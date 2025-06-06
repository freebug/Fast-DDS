// Copyright 2020 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file DiscoveryDataBase.cpp
 *
 */

#include <mutex>
#include <set>

#include <fastdds/dds/log/Log.hpp>
#include <fastdds/rtps/common/EntityId_t.hpp>
#include <fastdds/rtps/common/GuidPrefix_t.hpp>
#include <fastdds/rtps/common/RemoteLocators.hpp>
#include <statistics/rtps/GuidUtils.hpp>

#include <rtps/builtin/discovery/database/DiscoveryDataBase.hpp>

#include <nlohmann/json.hpp>
#include <rtps/builtin/discovery/database/backup/SharedBackupFunctions.hpp>

namespace eprosima {
namespace fastdds {
namespace rtps {
namespace ddb {

using ParticipantState = DiscoveryParticipantsAckStatus::ParticipantState;

DiscoveryDataBase::DiscoveryDataBase(
        const fastdds::rtps::GuidPrefix_t& server_guid_prefix)
    : server_guid_prefix_(server_guid_prefix)
    , server_acked_by_all_(true)
    , enabled_(true)
    , new_updates_(0)
    , processing_backup_(false)
    , is_persistent_(false)
{
}

DiscoveryDataBase::~DiscoveryDataBase()
{
    if (!clear().empty())
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Destroying a NOT cleared database");
    }

    if (is_persistent_)
    {
        backup_file_.close();
    }
}

void DiscoveryDataBase::add_server(
        fastdds::rtps::GuidPrefix_t server)
{
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Server " << server << " added");
    servers_.insert(server);
}

void DiscoveryDataBase::remove_server(
        fastdds::rtps::GuidPrefix_t server)
{
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (servers_.erase(server) == 1)
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Removing server " << server);
    }
    else
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Tried to remove " << server << " but it was not found");
    }
}

void DiscoveryDataBase::remove_related_alive_from_history_nts(
        fastdds::rtps::WriterHistory* writer_history,
        const fastdds::rtps::GuidPrefix_t& entity_guid_prefix)
{
    // Iterate over changes in writer_history
    for (auto chit = writer_history->changesBegin(); chit != writer_history->changesEnd();)
    {
        // Remove all DATA whose original sender was entity_guid_prefix from writer_history
        if (entity_guid_prefix == guid_from_change(*chit).guidPrefix)
        {
            chit = writer_history->remove_change(chit, false);
            continue;
        }
        chit++;
    }
}

std::vector<fastdds::rtps::CacheChange_t*> DiscoveryDataBase::clear()
{
    // Cannot clear an enabled database, since there could be inconsistencies after the process
    if (enabled_)
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Cannot clear an enabled database");
        return std::vector<fastdds::rtps::CacheChange_t*>({});
    }
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Clearing DiscoveryDataBase");

    std::lock_guard<std::recursive_mutex> guard(mutex_);

    /* Clear receive queues. Set changes inside to release */
    while (!pdp_data_queue_.Empty())
    {
        // This moves the value, do not copy it
        DiscoveryPDPDataQueueInfo data_queue_info = pdp_data_queue_.FrontAndPop();
        changes_to_release_.push_back(data_queue_info.change());
    }
    pdp_data_queue_.Clear(

        );
    while (!edp_data_queue_.Empty())
    {
        // This moves the value, do not copy it
        DiscoveryEDPDataQueueInfo data_queue_info = edp_data_queue_.FrontAndPop();
        changes_to_release_.push_back(data_queue_info.change());
    }
    edp_data_queue_.Clear();

    /* Clear by_topic collections */
    writers_by_topic_.clear();
    readers_by_topic_.clear();

    /* Clear list of dirty topics */
    dirty_topics_.clear();

    /* Clear disposals list */
    disposals_.clear();

    /* Clear to_send collections */
    pdp_to_send_.clear();
    edp_publications_to_send_.clear();
    edp_subscriptions_to_send_.clear();

    /* Clear writers_ */
    for (auto writers_it = writers_.begin(); writers_it != writers_.end();)
    {
        writers_it = delete_writer_entity_(writers_it);
    }

    /* Clear readers_ */
    for (auto readers_it = readers_.begin(); readers_it != readers_.end();)
    {
        readers_it = delete_reader_entity_(readers_it);
    }

    /* Clear participants_ */
    for (auto participants_it = participants_.begin(); participants_it != participants_.end();)
    {
        participants_it = delete_participant_entity_(participants_it);
    }

    /* Reset state parameters */
    server_acked_by_all_ = true;

    /* Clear changes to release */
    std::vector<fastdds::rtps::CacheChange_t*> leftover_changes = changes_to_release_;
    changes_to_release_.clear();
    servers_.clear();

    /* Return the collection of changes that are no longer owned by the database */
    return leftover_changes;
}

bool DiscoveryDataBase::pdp_is_relevant(
        const eprosima::fastdds::rtps::CacheChange_t& change,
        const eprosima::fastdds::rtps::GUID_t& reader_guid) const
{

    // Get identity of the participant that generated the DATA(p|Up)
    fastdds::rtps::GuidPrefix_t change_guid_prefix = guid_from_change(&change).guidPrefix;

    // Own DATA(p|Up) is always relevant for remote PDP readers. Server's PDP ReaderProxy will never
    // be queried for relevance, since Participant's own PDP writer and reader are not matched,
    // and there for there is no ReaderProxy for participant's own PDP reader.
    if (server_guid_prefix_ == change_guid_prefix)
    {
        return true;
    }

    // Lock(shared mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);

    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "PDP " << change.instanceHandle << " is relevant to " << reader_guid);

    auto it = participants_.find(change_guid_prefix);
    if (it != participants_.end())
    {
        // it is relevant if the ack has not been received yet
        // in NOT_ALIVE case the set_disposal unmatches every participant
        return (it->second.is_relevant_participant(reader_guid.guidPrefix) &&
               !it->second.is_matched(reader_guid.guidPrefix));
    }
    // Not relevant
    return false;
}

bool DiscoveryDataBase::edp_publications_is_relevant(
        const eprosima::fastdds::rtps::CacheChange_t& change,
        const eprosima::fastdds::rtps::GUID_t& reader_guid) const
{
    // Get identity of the participant that generated the DATA
    fastdds::rtps::GUID_t change_guid = guid_from_change(&change);

    // Lock(shared mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);

    auto itp = participants_.find(change_guid.guidPrefix);
    if (itp == participants_.end())
    {
        // not relevant
        return false;
    }
    else if (!itp->second.is_matched(reader_guid.guidPrefix))
    {
        // not relevant
        return false;
    }

    auto itw = writers_.find(change_guid);
    if (itw != writers_.end())
    {
        // it is relevant if the ack has not been received yet
        return (itw->second.is_relevant_participant(reader_guid.guidPrefix) &&
               !itw->second.is_matched(reader_guid.guidPrefix));
    }
    // not relevant
    return false;
}

bool DiscoveryDataBase::edp_subscriptions_is_relevant(
        const eprosima::fastdds::rtps::CacheChange_t& change,
        const eprosima::fastdds::rtps::GUID_t& reader_guid) const
{
    // Get identity of the participant that generated the DATA
    fastdds::rtps::GUID_t change_guid = guid_from_change(&change);

    // Lock(shared mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);

    auto itp = participants_.find(change_guid.guidPrefix);
    if (itp == participants_.end())
    {
        // not relevant
        return false;
    }
    else if (!itp->second.is_matched(reader_guid.guidPrefix))
    {
        // not relevant
        return false;
    }

    auto itr = readers_.find(change_guid);
    if (itr != readers_.end())
    {
        // it is relevant if the ack has not been received yet
        return (itr->second.is_relevant_participant(reader_guid.guidPrefix) &&
               !itr->second.is_matched(reader_guid.guidPrefix));
    }
    // not relevant
    return false;
}

void DiscoveryDataBase::update_change_and_unmatch_(
        fastdds::rtps::CacheChange_t* new_change,
        ddb::DiscoverySharedInfo& entity)
{
    changes_to_release_.push_back(entity.update_and_unmatch(new_change));
    // Manually set relevant participants ACK status of this server, and of the participant that sent the
    // change, to 1. This way, we avoid backprogation of the data.
    entity.add_or_update_ack_participant(server_guid_prefix_, ParticipantState::ACKED);
    entity.add_or_update_ack_participant(new_change->writerGUID.guidPrefix, ParticipantState::ACKED);
}

void DiscoveryDataBase::add_ack_(
        const eprosima::fastdds::rtps::CacheChange_t* change,
        const eprosima::fastdds::rtps::GuidPrefix_t& acked_entity)
{
    if (!enabled_)
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Discovery Database is disabled");
        return;
    }

    if (is_participant(change))
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE,
                "Adding DATA(p) ACK for change " << change->instanceHandle << " to " << acked_entity);
        auto it = participants_.find(guid_from_change(change).guidPrefix);
        if (it != participants_.end())
        {
            // Only add ACK if the change in the database is the same as the incoming change. Else, the change in the
            // database has been updated, so this ACK is not relevant anymore
            if (it->second.change()->write_params.sample_identity() == change->write_params.sample_identity())
            {
                it->second.add_or_update_ack_participant(acked_entity, ParticipantState::ACKED);
            }
        }
    }
    else if (is_writer(change))
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE,
                "Adding DATA(w) ACK for change " << change->instanceHandle << " to " << acked_entity);
        auto it = writers_.find(guid_from_change(change));
        if (it != writers_.end())
        {
            // Only add ACK if the change in the database is the same as the incoming change. Else, the change in the
            // database has been updated, so this ACK is not relevant anymore
            if (it->second.change()->write_params.sample_identity() == change->write_params.sample_identity())
            {
                it->second.add_or_update_ack_participant(acked_entity, ParticipantState::ACKED);
            }
        }
    }
    else if (is_reader(change))
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE,
                "Adding DATA(r) ACK for change " << change->instanceHandle << " to " << acked_entity);
        auto it = readers_.find(guid_from_change(change));
        if (it != readers_.end())
        {
            // Only add ACK if the change in the database is the same as the incoming change. Else, the change in the
            // database has been updated, so this ACK is not relevant anymore
            if (it->second.change()->write_params.sample_identity() == change->write_params.sample_identity())
            {
                it->second.add_or_update_ack_participant(acked_entity, ParticipantState::ACKED);
            }
        }
    }
}

bool DiscoveryDataBase::update(
        eprosima::fastdds::rtps::CacheChange_t* change,
        DiscoveryParticipantChangeData participant_change_data)
{
    // In case the ddb is persistent, we store every cache in queue in a file
    // The own server changes are not stored
    if (is_persistent_ && guid_from_change(change).guidPrefix != server_guid_prefix_)
    {
        // Does not allow to the server to erase the ddb before this message has been processed
        std::lock_guard<std::recursive_mutex> guard(data_queues_mutex_);
        nlohmann::json j;
        ddb::to_json(j, *change);
        backup_file_ << j;
        backup_file_.flush();
    }

    if (!enabled_)
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Discovery Database is disabled");
        return false;
    }

    if (!is_participant(change))
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Change is not a DATA(p|Up): " << change->instanceHandle);
        return false;
    }
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Adding DATA(p|Up) to the queue: " << change->instanceHandle);
    // Add the DATA(p|Up) to the PDP queue to process
    pdp_data_queue_.Push(eprosima::fastdds::rtps::ddb::DiscoveryPDPDataQueueInfo(change, participant_change_data));
    return true;
}

bool DiscoveryDataBase::update(
        eprosima::fastdds::rtps::CacheChange_t* change,
        const std::string& topic_name)
{
    // in case the ddb is persistent, we store every cache in queue in a file
    if (is_persistent_ && guid_from_change(change).guidPrefix != server_guid_prefix_)
    {
        // Does not allow to the server to erase the ddb before this message has been process
        std::lock_guard<std::recursive_mutex> guard(data_queues_mutex_);
        nlohmann::json j;
        ddb::to_json(j, *change);
        backup_file_ << j;
        backup_file_.flush();
    }

    if (!enabled_)
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Discovery Database is disabled");
        return false;
    }

    if (!is_writer(change) && !is_reader(change))
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Change is not a DATA(w|Uw|r|Ur): " << change->instanceHandle);
        return false;
    }

    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Adding DATA(w|Uw|r|Ur) to the queue: " << change->instanceHandle);
    //  add the DATA(w|Uw|r|Ur) to the EDP queue to process
    edp_data_queue_.Push(eprosima::fastdds::rtps::ddb::DiscoveryEDPDataQueueInfo(change, topic_name));
    return true;
}

const std::vector<eprosima::fastdds::rtps::CacheChange_t*> DiscoveryDataBase::changes_to_dispose()
{
    // lock(sharing mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return disposals_;
}

void DiscoveryDataBase::clear_changes_to_dispose()
{
    // lock(exclusive mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    disposals_.clear();
}

////////////
// Functions to process_to_send_lists()
const std::vector<eprosima::fastdds::rtps::CacheChange_t*> DiscoveryDataBase::pdp_to_send()
{
    // lock(sharing mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return pdp_to_send_;
}

void DiscoveryDataBase::clear_pdp_to_send()
{
    // lock(exclusive mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    pdp_to_send_.clear();
}

const std::vector<eprosima::fastdds::rtps::CacheChange_t*> DiscoveryDataBase::edp_publications_to_send()
{
    // lock(sharing mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return edp_publications_to_send_;
}

void DiscoveryDataBase::clear_edp_publications_to_send()
{
    // lock(exclusive mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    edp_publications_to_send_.clear();
}

const std::vector<eprosima::fastdds::rtps::CacheChange_t*> DiscoveryDataBase::edp_subscriptions_to_send()
{
    // lock(sharing mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return edp_subscriptions_to_send_;
}

void DiscoveryDataBase::clear_edp_subscriptions_to_send()
{
    // lock(exclusive mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    edp_subscriptions_to_send_.clear();
}

const std::vector<eprosima::fastdds::rtps::CacheChange_t*> DiscoveryDataBase::changes_to_release()
{
    // lock(sharing mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return changes_to_release_;
}

void DiscoveryDataBase::clear_changes_to_release()
{
    // lock(exclusive mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    changes_to_release_.clear();
}

////////////
// Functions to process PDP and EDP data queues
void DiscoveryDataBase::process_pdp_data_queue()
{
    if (!enabled_)
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Discovery Database is disabled");
        return;
    }

    // Lock(exclusive mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);

    // Process all messages in the queque
    while (!pdp_data_queue_.Empty())
    {
        // Process each message with FrontAndPop(). Move it, do not copy it
        DiscoveryPDPDataQueueInfo data_queue_info = pdp_data_queue_.FrontAndPop();

        // If the change is a DATA(p)
        if (data_queue_info.change()->kind == eprosima::fastdds::rtps::ALIVE)
        {
            // Update participants map
            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "DATA(p) of entity " << data_queue_info.change()->instanceHandle <<
                    " received from: " << data_queue_info.change()->writerGUID);
            create_participant_from_change_(data_queue_info.change(), data_queue_info.participant_change_data());
        }
        // If the change is a DATA(Up)
        else
        {
            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "DATA(Up) of entity " << data_queue_info.change()->instanceHandle <<
                    " received from: " << data_queue_info.change()->writerGUID);
            process_dispose_participant_(data_queue_info.change());
        }
    }
}

bool DiscoveryDataBase::process_edp_data_queue()
{
    if (!enabled_)
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Discovery Database is disabled");
        return false;
    }

    bool is_dirty_topic = false;

    // Lock(exclusive mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);

    eprosima::fastdds::rtps::CacheChange_t* change;
    std::string topic_name;

    // Process all messages in the queque
    while (!edp_data_queue_.Empty())
    {
        // Process each message with FrontAndPop(). Move it, do not copy it
        DiscoveryEDPDataQueueInfo data_queue_info = edp_data_queue_.FrontAndPop();
        change = data_queue_info.change();
        topic_name = data_queue_info.topic();

        // If the change is a DATA(w|r)
        if (change->kind == eprosima::fastdds::rtps::ALIVE)
        {
            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "ALIVE change received from: " << change->instanceHandle);
            // DATA(w) case
            if (is_writer(change))
            {
                EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "DATA(w) in topic " << topic_name << " received from: "
                                                                          << change->instanceHandle);
                create_writers_from_change_(change, topic_name);
            }
            // DATA(r) case
            else if (is_reader(change))
            {
                EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "DATA(r) in topic " << topic_name << " received from: "
                                                                          << change->instanceHandle);
                create_readers_from_change_(change, topic_name);
            }
        }
        // If the change is a DATA(Uw|Ur)
        else
        {
            // DATA(Uw) case
            if (is_writer(change))
            {
                EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "DATA(Uw) received from: " << change->instanceHandle);
                process_dispose_writer_(change);
            }
            // DATA(Ur) case
            else if (is_reader(change))
            {
                EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "DATA(Ur) received from: " << change->instanceHandle);
                process_dispose_reader_(change);
            }
        }
    }

    return is_dirty_topic;
}

void DiscoveryDataBase::create_participant_from_change_(
        eprosima::fastdds::rtps::CacheChange_t* ch,
        const DiscoveryParticipantChangeData& change_data)
{
    fastdds::rtps::GUID_t change_guid = guid_from_change(ch);
    auto participant_it = participants_.find(change_guid.guidPrefix);

    // The participant was already known in the database
    if (participant_it != participants_.end())
    {
        update_participant_from_change_(participant_it->second, ch, change_data);
    }

    // New participant
    else
    {
        create_new_participant_from_change_(ch, change_data);
    }
}

void DiscoveryDataBase::match_new_server_(
        eprosima::fastdds::rtps::GuidPrefix_t& participant_prefix,
        bool is_superclient)
{
    // Send Our DATA(p) to the new participant.
    // If this is not done, our data could be skipped afterwards because of a gap sent in newer DATA(p)s,
    // so the new participant could never receive our data
    auto our_data_it = participants_.find(server_guid_prefix_);
    assert(our_data_it != participants_.end());
    add_pdp_to_send_(our_data_it->second.change());

    if (!is_superclient)
    {
        // To obtain a mesh topology with servers, we need to:
        // - Make all known servers relevant to the new server
        // - Make the new server relevant to all known servers
        // - Send DATA(p) of all known servers to the new server
        // - Send Data(p) of the new server to all other servers
        for (auto& part : participants_)
        {
            if (part.first != server_guid_prefix_ && !part.second.is_client() && !part.second.is_superclient())
            {
                if (part.first == participant_prefix)
                {
                    std::lock_guard<std::recursive_mutex> guard(mutex_);
                    bool resend_new_pdp = false;
                    for (auto& server: servers_)
                    {
                        if (server != participant_prefix)
                        {
                            // Make all known servers relevant to the new server, but not matched
                            part.second.add_or_update_ack_participant(server, ParticipantState::PENDING_SEND);
                            resend_new_pdp = true;
                        }
                    }
                    if (resend_new_pdp)
                    {
                        // Send DATA(p) of the new server to all other servers.
                        add_pdp_to_send_(part.second.change());
                    }
                }
                else
                {
                    // Make the new server relevant to all known servers
                    part.second.add_or_update_ack_participant(participant_prefix, ParticipantState::PENDING_SEND);
                    // Send DATA(p) of all known servers to the new participant
                    add_pdp_to_send_(part.second.change());
                }
            }
        }
    }
    // The resources needed for TCP new connections are created during the matching process when the
    // DATA(p) is received by each server.

    // Create virtual endpoints
    create_virtual_endpoints_(participant_prefix);
}

void DiscoveryDataBase::create_virtual_endpoints_(
        eprosima::fastdds::rtps::GuidPrefix_t& participant_prefix)
{
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Creating virtual entities for " << participant_prefix);
    /* Create virtual writer */
    // Create a GUID for the virtual writer from the local server GUID prefix and the virtual writer entity
    // ID.
    fastdds::rtps::GUID_t virtual_writer_guid(participant_prefix,
            fastdds::rtps::ds_server_virtual_writer);
    // Create a populate the Cache Change with the necessary information.
    fastdds::rtps::CacheChange_t* virtual_writer_change = new fastdds::rtps::CacheChange_t();
    virtual_writer_change->kind = fastdds::rtps::ChangeKind_t::ALIVE;
    virtual_writer_change->writerGUID.guidPrefix = participant_prefix;
    virtual_writer_change->writerGUID.entityId = fastdds::rtps::ds_server_virtual_writer;
    virtual_writer_change->instanceHandle = fastdds::rtps::InstanceHandle_t(virtual_writer_guid);
    // Populate sample identity
    fastdds::rtps::SampleIdentity virtual_writer_sample_id;
    virtual_writer_sample_id.writer_guid(virtual_writer_guid);
    virtual_writer_sample_id.sequence_number(eprosima::fastdds::rtps::SequenceNumber_t(0));
    // Set write params
    eprosima::fastdds::rtps::WriteParams virtual_writer_writer_params;
    virtual_writer_writer_params.sample_identity(virtual_writer_sample_id);
    virtual_writer_writer_params.related_sample_identity(virtual_writer_sample_id);
    virtual_writer_change->write_params = std::move(virtual_writer_writer_params);
    virtual_writer_change->writer_info.previous = nullptr;
    virtual_writer_change->writer_info.next = nullptr;
    virtual_writer_change->writer_info.num_sent_submessages = 0;
    // Create the virtual writer
    create_writers_from_change_(virtual_writer_change, virtual_topic_);

    /* Create virtual reader */
    // Create a GUID for the virtual reader from the local server GUID prefix and the virtual reader entity
    // ID.
    fastdds::rtps::GUID_t virtual_reader_guid(participant_prefix,
            fastdds::rtps::ds_server_virtual_reader);
    // Create a populate the Cache Change with the necessary information.
    fastdds::rtps::CacheChange_t* virtual_reader_change = new fastdds::rtps::CacheChange_t();
    virtual_reader_change->kind = fastdds::rtps::ChangeKind_t::ALIVE;
    virtual_reader_change->writerGUID.guidPrefix = participant_prefix;
    virtual_reader_change->writerGUID.entityId = fastdds::rtps::ds_server_virtual_reader;
    virtual_reader_change->instanceHandle = fastdds::rtps::InstanceHandle_t(virtual_reader_guid);
    // Populate sample identity
    fastdds::rtps::SampleIdentity virtual_reader_sample_id;
    virtual_reader_sample_id.writer_guid(virtual_reader_guid);
    virtual_reader_sample_id.sequence_number(eprosima::fastdds::rtps::SequenceNumber_t(0));
    // Set write params
    eprosima::fastdds::rtps::WriteParams virtual_reader_writer_params;
    virtual_reader_writer_params.sample_identity(virtual_reader_sample_id);
    virtual_reader_writer_params.related_sample_identity(virtual_reader_sample_id);
    virtual_reader_change->write_params = std::move(virtual_reader_writer_params);
    virtual_reader_change->writer_info.previous = nullptr;
    virtual_reader_change->writer_info.next = nullptr;
    virtual_reader_change->writer_info.num_sent_submessages = 0;
    // Create the virtual reader
    create_readers_from_change_(virtual_reader_change, virtual_topic_);
}

bool DiscoveryDataBase::participant_data_has_changed_(
        const DiscoveryParticipantInfo& participant_info,
        const DiscoveryParticipantChangeData& new_change_data)
{
    return !(participant_info.is_local() == new_change_data.is_local() &&
           participant_info.is_client() == new_change_data.is_client() &&
           participant_info.is_superclient() == new_change_data.is_superclient());
}

void DiscoveryDataBase::create_new_participant_from_change_(
        eprosima::fastdds::rtps::CacheChange_t* ch,
        const DiscoveryParticipantChangeData& change_data)
{
    fastdds::rtps::GUID_t change_guid = guid_from_change(ch);

    std::pair<std::map<eprosima::fastdds::rtps::GuidPrefix_t, DiscoveryParticipantInfo>::iterator, bool> ret =
            participants_.insert(
        std::make_pair(
            change_guid.guidPrefix,
            DiscoveryParticipantInfo(ch, server_guid_prefix_, change_data)));

    // If insert was successful
    if (ret.second)
    {
        // New participant found
        new_updates_++;

        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "New participant added: " << change_guid.guidPrefix);

        // Manually set to 1 the relevant participants ACK status of the participant that sent the change. This way,
        // we avoid backprogation of the data.
        ret.first->second.add_or_update_ack_participant(ch->writerGUID.guidPrefix, ParticipantState::ACKED);

        // If the DATA(p) it's from this server, it is already in history and we do nothing here
        if (change_guid.guidPrefix != server_guid_prefix_)
        {
            // If the participant is a new participant, mark that not everyone has ACKed this server's DATA(p).
            // TODO if the new participant is a server it may be that our DATA(p) is already acked because he is
            //  our server and we have pinged it. But also if we are its server it could be the case that
            //  our DATA(p) is not acked even when it is our server. Solution: see in PDPServer how the change has
            //  arrived, if because our ping or because their DATA(p). MINOR PROBLEM
            server_acked_by_all(false);
        }

        // If it is local and server we have to create virtual endpoints, except for our own server
        if (change_guid.guidPrefix != server_guid_prefix_ &&
                !ret.first->second.is_client() && ret.first->second.is_local())
        {
            // Match new server and create virtual endpoints
            match_new_server_(change_guid.guidPrefix, change_data.is_superclient());
        }
    }
    else
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Failed adding new participant " << change_guid.guidPrefix);
    }
}

void DiscoveryDataBase::update_participant_from_change_(
        DiscoveryParticipantInfo& participant_info,
        eprosima::fastdds::rtps::CacheChange_t* ch,
        const DiscoveryParticipantChangeData& change_data)
{
    fastdds::rtps::GUID_t change_guid = guid_from_change(ch);

    assert(ch->kind == eprosima::fastdds::rtps::ALIVE);

    // If the change corresponds to a previously removed participant (which hasn't yet been removed from the map since
    // the DATA(Up) is still unacked), update map with new data and behave as if it was a new participant.
    // Remove also the old change from the disposals collection, if it was added just before
    if (participant_info.change()->kind != eprosima::fastdds::rtps::ALIVE)
    {
        // Update the change data
        participant_info.participant_change_data(change_data);

        // Remove old change from disposals if it was added just before to avoid sending data UP
        auto it = std::find(disposals_.begin(), disposals_.end(), participant_info.change());
        if (it != disposals_.end())
        {
            disposals_.erase(it);
        }

        // Update change. This should add the UNALIVE change to changes_to_release_, which should later both remove the
        // change from the writer's history and release the change
        update_change_and_unmatch_(ch, participant_info);

        // If it is local and server we have to create virtual endpoints, except for our own server
        if (change_guid.guidPrefix != server_guid_prefix_ && !change_data.is_client() && change_data.is_local())
        {
            // Match new server and create virtual endpoints
            // NOTE: match after having updated the change, so virtual endpoints are not discarded for having
            // an associated unalive participant
            match_new_server_(change_guid.guidPrefix, change_data.is_superclient());
        }

        // Treat as a new participant found
        new_updates_++;
        if (change_guid.guidPrefix != server_guid_prefix_)
        {
            server_acked_by_all(false);
        }
    }

    // Specific case when a Data(P) from an entity A known as remote comes from the very entity A (we have
    // the Data(P) because of other server B, but now it arrives from A itself)
    // The entity A changes to local
    // Must be local data, or else it is a remote endpoint and should not be changed
    else if (change_guid.guidPrefix != server_guid_prefix_ && change_data.is_local() &&
            DiscoveryDataBase::participant_data_has_changed_(participant_info, change_data))
    {
        // Update the change data
        participant_info.participant_change_data(change_data);

        // Update change
        update_change_and_unmatch_(ch, participant_info);

        // If the participant changes to server local, virtual endpoints must be added
        // If it is local and server the only possibility is it was a remote server and it must be converted to local
        if (!change_data.is_client())
        {
            // NOTE: match after having updated the change in order to send the new Data(P)
            match_new_server_(change_guid.guidPrefix, change_data.is_superclient());
        }

        // Treat as a new participant found
        new_updates_++;
        server_acked_by_all(false);

        // It is possible that this Data(P) is in our history if it has not been acked by all
        // In this case we have to resend it with the new update
        if (!participant_info.is_acked_by_all())
        {
            add_pdp_to_send_(ch);
        }
    }

    // Only update database if the change is newer than the one we already have
    else if (ch->write_params.sample_identity().sequence_number() >
            participant_info.change()->write_params.sample_identity().sequence_number())
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Participant already known with newer sequence number");

        // The change could be newer and at the same time not being an update.
        // This happens with DATAs coming from servers, since they take their own DATAs in and out frequently,
        // so the sequence number in `write_params` changes.
        // To account for that, we discard the DATA if the payload is exactly the same as what we have.
        if (!(ch->serializedPayload == participant_info.change()->serializedPayload))
        {
            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Participant updating. Marking old change to release");
            // Update participant's change in the database, set all relevant participants ACK status to 0, and add
            // old change to changes_to_release_.
            update_change_and_unmatch_(ch, participant_info);

            // If it is an update of our own server, is already in history
            // Else, it needs to be sent in case it has unacked participants
            if (change_guid.guidPrefix != server_guid_prefix_ &&
                    !participant_info.is_acked_by_all())
            {
                new_updates_++;
                add_pdp_to_send_(ch);
            }
        }
    }

    // if the cache is not new we have to release it, because it is repeated or outdated
    else
    {
        // if the change is the same that we already have, we update the ack list. This is because we have
        //  received the data from two servers, so we have to set that both of them already know this data
        if (ch->write_params.sample_identity().sequence_number() ==
                participant_info.change()->write_params.sample_identity().sequence_number())
        {
            participant_info.add_or_update_ack_participant(ch->writerGUID.guidPrefix, ParticipantState::ACKED);
        }

        // we release it if it's the same or if it is lower
        changes_to_release_.push_back(ch);
    }
}

void DiscoveryDataBase::create_writers_from_change_(
        eprosima::fastdds::rtps::CacheChange_t* ch,
        const std::string& topic_name)
{
    const eprosima::fastdds::rtps::GUID_t& writer_guid = guid_from_change(ch);
    auto writer_it = writers_.find(writer_guid);

    // The writer was already known in the database
    if (writer_it != writers_.end())
    {
        // Only update database if the change is newer than the one we already have
        if (ch->write_params.sample_identity().sequence_number() >
                writer_it->second.change()->write_params.sample_identity().sequence_number())
        {
            // The change could be newer and at the same time not being an update.
            // This happens with DATAs coming from servers, since they take their own DATAs in and out frequently,
            // so the sequence number in `write_params` changes.
            // To account for that, we discard the DATA if the payload is exactly the same as what we have.
            if (!(ch->serializedPayload == writer_it->second.change()->serializedPayload))
            {
                // Update the change related to the writer and return the old change to the pool
                // TODO (Paris): when updating, be careful of not to do unmatch if the only endpoint in the other
                // participant is NOT ALIVE. This means that you still have to send your Data(Ux) to him but not the
                // updates
                update_change_and_unmatch_(ch, writer_it->second);

                // It needs to be sent in case it has unacked participants
                if (!writer_it->second.is_acked_by_all())
                {
                    new_updates_++;
                    add_edp_publications_to_send_(ch);
                }
            }
        }
        // If the cache is not new we have to release it, because it is repeated or outdated
        else
        {
            // If the change is the same that we already have, we update the ack list. This is because we have
            // received the data from two servers, so we have to set that both of them already know this data
            if (ch->write_params.sample_identity().sequence_number() ==
                    writer_it->second.change()->write_params.sample_identity().sequence_number())
            {
                writer_it->second.add_or_update_ack_participant(ch->writerGUID.guidPrefix, ParticipantState::ACKED);
            }

            // we release it if it's the same or if it is lower
            changes_to_release_.push_back(ch);
        }
    }
    // The writer was NOT known by the database
    else
    {
        // Check if corresponding participant is known, abort otherwise
        // NOTE: Processing a DATA(w) should always be preceded by the reception and processing of its corresponding
        // participant. However, one may receive a DATA(w) just after the participant has been removed, case in which the
        // former should no longer be processed.
        std::map<eprosima::fastdds::rtps::GuidPrefix_t, DiscoveryParticipantInfo>::iterator writer_part_it =
                participants_.find(writer_guid.guidPrefix);
        if (writer_part_it == participants_.end())
        {
            EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE,
                    "Writer " << writer_guid << " has no associated participant. Skipping");
            assert(topic_name != virtual_topic_);
            changes_to_release_.push_back(ch); // Release change so it can be reused
            return;
        }
        else if (writer_part_it->second.change()->kind != fastdds::rtps::ChangeKind_t::ALIVE)
        {
            EPROSIMA_LOG_WARNING(DISCOVERY_DATABASE,
                    "Writer " << writer_guid << " is associated to a removed participant. Skipping");
            assert(topic_name != virtual_topic_);
            changes_to_release_.push_back(ch); // Release change so it can be reused
            return;
        }

        // Add entry to writers_
        DiscoveryEndpointInfo tmp_writer(
            ch,
            topic_name,
            topic_name == virtual_topic_,
            server_guid_prefix_);

        std::pair<std::map<eprosima::fastdds::rtps::GUID_t, DiscoveryEndpointInfo>::iterator, bool> ret =
                writers_.insert(std::make_pair(writer_guid, tmp_writer));
        if (!ret.second)
        {
            EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Error inserting writer " << writer_guid);
            return;
        }
        writer_it = ret.first;

        // New writer found
        new_updates_++;

        // Add entry to participants_[guid_prefix]::writers
        writer_part_it->second.add_writer(writer_guid);

        // Add writer to writers_by_topic_[topic_name]
        add_writer_to_topic_(writer_guid, topic_name);

        // Manually set to 1 the relevant participants ACK status of the participant that sent the change. This way,
        // we avoid backprogation of the data.
        writer_it->second.add_or_update_ack_participant(ch->writerGUID.guidPrefix, ParticipantState::ACKED);

        // if topic is virtual, it must iterate over all readers
        if (topic_name == virtual_topic_)
        {
            for (auto reader_it : readers_)
            {
                match_writer_reader_(writer_guid, reader_it.first);
            }
        }
        else
        {
            auto readers_it = readers_by_topic_.find(topic_name);
            if (readers_it == readers_by_topic_.end())
            {
                EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Topic error: " << topic_name << ". Must exist.");
                return;
            }
            for (auto reader : readers_it->second)
            {
                match_writer_reader_(writer_guid, reader);
            }
        }
        // Update set of dirty_topics
        set_dirty_topic_(topic_name);
    }
}

void DiscoveryDataBase::create_readers_from_change_(
        eprosima::fastdds::rtps::CacheChange_t* ch,
        const std::string& topic_name)
{
    const eprosima::fastdds::rtps::GUID_t& reader_guid = guid_from_change(ch);
    auto reader_it = readers_.find(reader_guid);

    // The reader was already known in the database
    if (reader_it != readers_.end())
    {
        // Only update database if the change is newer than the one we already have
        if (ch->write_params.sample_identity().sequence_number() >
                reader_it->second.change()->write_params.sample_identity().sequence_number())
        {
            // The change could be newer and at the same time not being an update.
            // This happens with DATAs coming from servers, since they take their own DATAs in and out frequently,
            // so the sequence number in `write_params` changes.
            // To account for that, we discard the DATA if the payload is exactly the same as what wee have.
            if (!(ch->serializedPayload == reader_it->second.change()->serializedPayload))
            {
                // Update the change related to the reader and return the old change to the pool
                // TODO (Paris): when updating, be careful of not to do unmatch if the only endpoint in the other
                // participant is NOT ALIVE. This means that you still have to send your Data(Ux) to him but not the
                // updates
                update_change_and_unmatch_(ch, reader_it->second);

                // It needs to be sent in case it has unacked participants
                if (!reader_it->second.is_acked_by_all())
                {
                    new_updates_++;
                    add_edp_subscriptions_to_send_(ch);
                }
            }
        }
        // If the cache is not new we have to release it, because it is repeated or outdated
        else
        {
            // if the change is the same that we already have, we update the ack list. This is because we have
            //  received the data from two servers, so we have to set that both of them already know this data
            if (ch->write_params.sample_identity().sequence_number() ==
                    reader_it->second.change()->write_params.sample_identity().sequence_number())
            {
                reader_it->second.add_or_update_ack_participant(ch->writerGUID.guidPrefix, ParticipantState::ACKED);
            }

            // we release it if it's the same or if it is lower
            changes_to_release_.push_back(ch);
        }
    }
    // The reader was NOT known by the database
    else
    {
        // Check if corresponding participant is known, abort otherwise
        // NOTE: Processing a DATA(r) should always be preceded by the reception and processing of its corresponding
        // participant. However, one may receive a DATA(r) just after the participant has been removed, case in which the
        // former should no longer be processed.
        std::map<eprosima::fastdds::rtps::GuidPrefix_t, DiscoveryParticipantInfo>::iterator reader_part_it =
                participants_.find(reader_guid.guidPrefix);
        if (reader_part_it == participants_.end())
        {
            EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE,
                    "Reader " << reader_guid << " has no associated participant. Skipping");
            assert(topic_name != virtual_topic_);
            changes_to_release_.push_back(ch); // Release change so it can be reused
            return;
        }
        else if (reader_part_it->second.change()->kind != fastdds::rtps::ChangeKind_t::ALIVE)
        {
            EPROSIMA_LOG_WARNING(DISCOVERY_DATABASE,
                    "Reader " << reader_guid << " is associated to a removed participant. Skipping");
            assert(topic_name != virtual_topic_);
            changes_to_release_.push_back(ch); // Release change so it can be reused
            return;
        }

        // Add entry to readers_
        DiscoveryEndpointInfo tmp_reader(
            ch,
            topic_name,
            topic_name == virtual_topic_,
            server_guid_prefix_);

        std::pair<std::map<eprosima::fastdds::rtps::GUID_t, DiscoveryEndpointInfo>::iterator, bool> ret =
                readers_.insert(std::make_pair(reader_guid, tmp_reader));
        if (!ret.second)
        {
            EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Error inserting reader " << reader_guid);
            return;
        }
        reader_it = ret.first;

        // New reader found
        new_updates_++;

        // Add entry to participants_[guid_prefix]::readers
        reader_part_it->second.add_reader(reader_guid);

        // Add reader to readers_by_topic_[topic_name]
        add_reader_to_topic_(reader_guid, topic_name);

        // Manually set to 1 the relevant participants ACK status of the participant that sent the change. This way,
        // we avoid backprogation of the data.
        reader_it->second.add_or_update_ack_participant(ch->writerGUID.guidPrefix, ParticipantState::ACKED);

        // If topic is virtual, it must iterate over all readers
        if (topic_name == virtual_topic_)
        {
            for (auto writer_it : writers_)
            {
                match_writer_reader_(writer_it.first, reader_guid);
            }
        }
        else
        {
            auto writers_it = writers_by_topic_.find(topic_name);
            if (writers_it == writers_by_topic_.end())
            {
                EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Topic error: " << topic_name << ". Must exist.");
                return;
            }
            for (auto writer : writers_it->second)
            {
                match_writer_reader_(writer, reader_guid);
            }
        }
        // Update set of dirty_topics
        set_dirty_topic_(topic_name);
    }
}

void DiscoveryDataBase::match_writer_reader_(
        const eprosima::fastdds::rtps::GUID_t& writer_guid,
        const eprosima::fastdds::rtps::GUID_t& reader_guid)
{
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Matching writer " << writer_guid << " with reader " << reader_guid);

    // writer entity
    auto wit = writers_.find(writer_guid);
    if (wit == writers_.end())
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Matching unexisting writer " << writer_guid);
        return;
    }
    DiscoveryEndpointInfo& writer_info = wit->second;

    // writer participant
    auto p_wit = participants_.find(writer_guid.guidPrefix);
    if (p_wit == participants_.end())
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Matching unexisting participant from writer " << writer_guid);
        return;
    }
    DiscoveryParticipantInfo& writer_participant_info = p_wit->second;

    // reader entity
    auto rit = readers_.find(reader_guid);
    if (rit == readers_.end())
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Matching unexisting reader " << reader_guid);
        return;
    }
    DiscoveryEndpointInfo& reader_info = rit->second;

    // reader participant
    auto p_rit = participants_.find(reader_guid.guidPrefix);
    if (p_rit == participants_.end())
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Matching unexisting participant from reader " << reader_guid);
        return;
    }
    DiscoveryParticipantInfo& reader_participant_info = p_rit->second;

    // virtual              - needs info and give none
    // local                - needs info and give info
    // external             - needs none and give info
    // writer needs info    = add writer participant in reader ack list
    // writer give info     = add reader participant in writer ack list

    // TODO reduce number of cases. This is more visual, but can be reduce joining them
    if (writer_info.is_virtual())
    {
        // Writer virtual

        // If reader is virtual OR not local, do not exchange info. Servers do not redirect Data(p) of remote clients.
        // Otherwise, writer needs all the info from this endpoint
        if (!reader_info.is_virtual() &&
                (reader_participant_info.is_local() || writer_participant_info.is_superclient()))
        {
            // Only if they do not have the info yet
            if (!reader_participant_info.is_relevant_participant(writer_guid.guidPrefix))
            {
                reader_participant_info.add_or_update_ack_participant(writer_guid.guidPrefix);
            }

            if (!reader_info.is_relevant_participant(writer_guid.guidPrefix))
            {
                reader_info.add_or_update_ack_participant(writer_guid.guidPrefix);
            }
        }
    }
    else if (writer_participant_info.is_local())
    {
        // Writer local

        if (reader_info.is_virtual())
        {
            // Reader virtual
            // Writer gives info to reader
            // Only if they do not have the info yet
            if (!writer_participant_info.is_relevant_participant(reader_guid.guidPrefix))
            {
                writer_participant_info.add_or_update_ack_participant(reader_guid.guidPrefix);
            }

            if (!writer_info.is_relevant_participant(reader_guid.guidPrefix))
            {
                writer_info.add_or_update_ack_participant(reader_guid.guidPrefix);
            }
        }
        else if (reader_participant_info.is_local())
        {
            // Reader local
            // Both exchange info
            // Only if they do not have the info yet
            if (!writer_participant_info.is_relevant_participant(reader_guid.guidPrefix))
            {
                writer_participant_info.add_or_update_ack_participant(reader_guid.guidPrefix);
            }

            if (!writer_info.is_relevant_participant(reader_guid.guidPrefix))
            {
                writer_info.add_or_update_ack_participant(reader_guid.guidPrefix);
            }

            if (!reader_participant_info.is_relevant_participant(writer_guid.guidPrefix))
            {
                reader_participant_info.add_or_update_ack_participant(writer_guid.guidPrefix);
            }

            if (!reader_info.is_relevant_participant(writer_guid.guidPrefix))
            {
                reader_info.add_or_update_ack_participant(writer_guid.guidPrefix);
            }
        }
        else
        {
            // Reader external
            // Reader gives info to writer
            // Only if they do not have the info yet
            if (!reader_participant_info.is_relevant_participant(writer_guid.guidPrefix))
            {
                reader_participant_info.add_or_update_ack_participant(writer_guid.guidPrefix);
            }

            if (!reader_info.is_relevant_participant(writer_guid.guidPrefix))
            {
                reader_info.add_or_update_ack_participant(writer_guid.guidPrefix);
            }
        }
    }
    else
    {
        // Writer external

        // If reader is external OR virtual, do not exchange info. Servers do not redirect Data(p) of remote clients.
        // Otherwise, reader needs all the info from this endpoint
        if (reader_participant_info.is_local() &&
                (!reader_info.is_virtual() || reader_participant_info.is_superclient()))
        {
            // Only if they do not have the info yet
            if (!writer_participant_info.is_relevant_participant(reader_guid.guidPrefix))
            {
                writer_participant_info.add_or_update_ack_participant(reader_guid.guidPrefix);
            }

            if (!writer_info.is_relevant_participant(reader_guid.guidPrefix))
            {
                writer_info.add_or_update_ack_participant(reader_guid.guidPrefix);
            }
        }
    }
}

bool DiscoveryDataBase::set_dirty_topic_(
        const std::string& topic)
{
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Setting topic " << topic << " as dirty");

    // If topic is virtual, we need to set as dirty all the other (non-virtual) topics
    if (topic == virtual_topic_)
    {
        // Set all topics to dirty
        dirty_topics_.clear();

        // It is enough to use writers_by_topic because the topics are simetrical in writers and readers:
        //  if a topic exists in one, it exists in the other
        for (auto topic_it : writers_by_topic_)
        {
            if (topic_it.first != virtual_topic_)
            {
                dirty_topics_.push_back(topic_it.first);
            }
        }
        return true;
    }
    else
    {
        if (std::find(
                    dirty_topics_.begin(),
                    dirty_topics_.end(),
                    topic) == dirty_topics_.end())
        {
            dirty_topics_.push_back(topic);
            return true;
        }
    }
    return false;
}

void DiscoveryDataBase::process_dispose_participant_(
        eprosima::fastdds::rtps::CacheChange_t* ch)
{
    const eprosima::fastdds::rtps::GUID_t& participant_guid = guid_from_change(ch);

    // Change DATA(p) with DATA(Up) in participants map
    std::map<eprosima::fastdds::rtps::GuidPrefix_t, DiscoveryParticipantInfo>::iterator pit =
            participants_.find(participant_guid.guidPrefix);
    if (pit != participants_.end())
    {
        // Check if this participant is already NOT ALIVE
        // Due to the way of announce a server, it is common to receive two DATA(Up) from the same server
        if (pit->second.change()->kind != fastdds::rtps::ChangeKind_t::ALIVE)
        {
            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Ignoring second DATA(Up)"
                    << participant_guid.guidPrefix);
            return;
        }
        // Only update DATA(p), leaving the change info untouched. This is because DATA(Up) does not have the
        // participant's meta-information, but we don't want to loose it here.
        update_change_and_unmatch_(ch, pit->second);

        // Any change in the entities known must be reported as a change in the discovery
        new_updates_++;
    }
    else
    {
        // This is not an error. It could be because we have already receive and process the DATA(Up)
        // from this participant and it is no longer in the database
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Processing disposal from an unexisting Participant"
                << participant_guid.guidPrefix);
        return;
    }

    // Delete entries from writers_ belonging to the participant
    while (!pit->second.writers().empty())
    {
        auto writer_guid = pit->second.writers().back();

        // Erase writer from topic
        unmatch_writer_(writer_guid);

        // Release the change and remove entity without setting Data(Uw)
        delete_writer_entity_(writer_guid);
    }

    // Delete entries from readers_ belonging to the participant
    while (!pit->second.readers().empty())
    {
        auto reader_guid = pit->second.readers().back();

        // This unmatch must erase the entity from writers
        unmatch_reader_(reader_guid);

        // Release the change and remove entity without setting Data(Ur)
        delete_reader_entity_(reader_guid);
    }

    // All participant endpoints must be already unmatched in others endpoints relevant_ack maps

    // Unmatch own participant
    unmatch_participant_(participant_guid.guidPrefix);

    // Add entry to disposals_
    if (std::find(disposals_.begin(), disposals_.end(), ch) == disposals_.end())
    {
        disposals_.push_back(ch);
    }
}

void DiscoveryDataBase::process_dispose_writer_(
        eprosima::fastdds::rtps::CacheChange_t* ch)
{
    const eprosima::fastdds::rtps::GUID_t& writer_guid = guid_from_change(ch);

    // Check if the writer is still alive (if DATA(Up) is processed before it will be erased)
    std::map<eprosima::fastdds::rtps::GUID_t, DiscoveryEndpointInfo>::iterator wit = writers_.find(writer_guid);
    if (wit != writers_.end())
    {
        // Change DATA(w) with DATA(Uw)
        update_change_and_unmatch_(ch, wit->second);

        // Remove writer from topic
        remove_writer_from_topic_(writer_guid, wit->second.topic());

        // Add entry to disposals_
        if (wit->second.topic() != virtual_topic_)
        {
            if (std::find(disposals_.begin(), disposals_.end(), ch) == disposals_.end())
            {
                disposals_.push_back(ch);
            }
        }

        // Any change in the entities known must be reported as a change in the discovery
        new_updates_++;
    }
}

void DiscoveryDataBase::process_dispose_reader_(
        eprosima::fastdds::rtps::CacheChange_t* ch)
{
    const eprosima::fastdds::rtps::GUID_t& reader_guid = guid_from_change(ch);

    // Check if the writer is still alive (if DATA(Up) is processed before it will be erased)

    std::map<eprosima::fastdds::rtps::GUID_t, DiscoveryEndpointInfo>::iterator rit = readers_.find(reader_guid);
    if (rit != readers_.end())
    {
        // Change DATA(r) with DATA(Ur)
        update_change_and_unmatch_(ch, rit->second);

        // Remove reader from topic
        remove_reader_from_topic_(reader_guid, rit->second.topic());

        // Add entry to disposals_
        if (rit->second.topic() != virtual_topic_)
        {
            if (std::find(disposals_.begin(), disposals_.end(), ch) == disposals_.end())
            {
                disposals_.push_back(ch);
            }
        }

        // Any change in the entities known must be reported as a change in the discovery
        new_updates_++;
    }
}

bool DiscoveryDataBase::process_dirty_topics()
{
    if (!enabled_)
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Discovery Database is disabled");
        return false;
    }

    // EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "process_dirty_topics start");
    // Get shared lock
    std::lock_guard<std::recursive_mutex> guard(mutex_);

    // Iterator objects are declared here because they are reused in each iteration of the loops
    std::map<eprosima::fastdds::rtps::GuidPrefix_t, DiscoveryParticipantInfo>::iterator parts_reader_it;
    std::map<eprosima::fastdds::rtps::GuidPrefix_t, DiscoveryParticipantInfo>::iterator parts_writer_it;
    std::map<eprosima::fastdds::rtps::GUID_t, DiscoveryEndpointInfo>::iterator readers_it;
    std::map<eprosima::fastdds::rtps::GUID_t, DiscoveryEndpointInfo>::iterator writers_it;

    // Iterate over dirty_topics_
    for (auto topic_it = dirty_topics_.begin(); topic_it != dirty_topics_.end();)
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Processing topic: " << *topic_it);
        // Flag to store whether a topic can be cleared.
        bool is_clearable = true;

        // Get all the writers in the topic
        std::vector<fastdds::rtps::GUID_t> writers;
        auto ret = writers_by_topic_.find(*topic_it);
        if (ret != writers_by_topic_.end())
        {
            writers = ret->second;
        }
        // Get all the readers in the topic
        std::vector<fastdds::rtps::GUID_t> readers;
        ret = readers_by_topic_.find(*topic_it);
        if (ret != readers_by_topic_.end())
        {
            readers = ret->second;
        }

        for (fastdds::rtps::GUID_t writer: writers)
        // Iterate over writers in the topic:
        {
            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "[" << *topic_it << "]" << " Processing writer: " << writer);
            // Iterate over readers in the topic:
            for (fastdds::rtps::GUID_t reader : readers)
            {
                EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "[" << *topic_it << "]" << " Processing reader: " << reader);
                // Find participants with writer info and participant with reader info in participants_
                parts_reader_it = participants_.find(reader.guidPrefix);
                parts_writer_it = participants_.find(writer.guidPrefix);

                // Check in `participants_` whether the client with the reader has acknowledge the PDP of the client
                // with the writer.
                if (parts_reader_it != participants_.end())
                {
                    if (parts_reader_it->second.is_matched(writer.guidPrefix))
                    {
                        // Find reader info in readers_
                        readers_it = readers_.find(reader);
                        // Check the status of the writer in `readers_[reader]::relevant_participants_builtin_ack_status`.
                        if (readers_it != readers_.end() &&
                                readers_it->second.is_relevant_participant(writer.guidPrefix) &&
                                !readers_it->second.is_waiting_ack(writer.guidPrefix))
                        {
                            // If the status is 0, add DATA(r) to a `edp_publications_to_send_` (if it's not there).
                            if (add_edp_subscriptions_to_send_(readers_it->second.change()))
                            {
                                EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Adding DATA(r) to send: "
                                        << readers_it->second.change()->instanceHandle);
                                readers_it->second.add_or_update_ack_participant(writer.guidPrefix,
                                        ParticipantState::WAITING_ACK);
                            }
                        }
                    }
                    else if (parts_reader_it->second.is_relevant_participant(writer.guidPrefix))
                    {
                        if (!parts_reader_it->second.is_waiting_ack(writer.guidPrefix))
                        {
                            // Add DATA(p) of the client with the writer to `pdp_to_send_` (if it's not there).
                            if (add_pdp_to_send_(parts_reader_it->second.change()))
                            {
                                EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Adding readers' DATA(p) to send: "
                                        << parts_reader_it->second.change()->instanceHandle);
                                parts_reader_it->second.add_or_update_ack_participant(writer.guidPrefix,
                                        ParticipantState::WAITING_ACK);
                            }
                        }
                        // Set topic as not-clearable.
                        is_clearable = false;
                    }
                }

                // Check in `participants_` whether the client with the writer has acknowledge the PDP of the client
                // with the reader.
                if (parts_writer_it != participants_.end())
                {
                    if (parts_writer_it->second.is_matched(reader.guidPrefix))
                    {
                        // Find writer info in writers_
                        writers_it = writers_.find(writer);
                        // Check the status of the reader in `writers_[writer]::relevant_participants_builtin_ack_status`.
                        if (writers_it != writers_.end() &&
                                writers_it->second.is_relevant_participant(reader.guidPrefix) &&
                                !writers_it->second.is_waiting_ack(reader.guidPrefix))
                        {
                            // If the status is 0, add DATA(w) to a `edp_subscriptions_to_send_` (if it's not there).
                            if (add_edp_publications_to_send_(writers_it->second.change()))
                            {
                                EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Adding DATA(w) to send: "
                                        << writers_it->second.change()->instanceHandle);
                                writers_it->second.add_or_update_ack_participant(reader.guidPrefix,
                                        ParticipantState::WAITING_ACK);
                            }
                        }
                    }
                    else if (parts_writer_it->second.is_relevant_participant(reader.guidPrefix))
                    {
                        if (!parts_writer_it->second.is_waiting_ack(reader.guidPrefix))
                        {
                            // Add DATA(p) of the client with the reader to `pdp_to_send_` (if it's not there).
                            if (add_pdp_to_send_(parts_writer_it->second.change()))
                            {
                                EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Adding writers' DATA(p) to send: "
                                        << parts_writer_it->second.change()->instanceHandle);
                                parts_writer_it->second.add_or_update_ack_participant(reader.guidPrefix,
                                        ParticipantState::WAITING_ACK);
                            }
                        }
                        // Set topic as not-clearable.
                        is_clearable = false;
                    }
                }
            }
        }

        // Check whether the topic is still dirty or it can be cleared
        if (is_clearable)
        {
            // Delete topic from dirty_topics_
            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Topic " << *topic_it << " has been cleaned");
            topic_it = dirty_topics_.erase(topic_it);
        }
        else
        {
            // Proceed with next topic
            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Topic " << *topic_it << " is still dirty");
            ++topic_it;
        }
    }

    // Return whether there still are dirty topics
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Are there dirty topics? " << !dirty_topics_.empty());

    return !dirty_topics_.empty();
}

bool DiscoveryDataBase::delete_entity_of_change(
        fastdds::rtps::CacheChange_t* change)
{
    if (!enabled_)
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Discovery Database is disabled");
        return false;
    }

    // Lock(exclusive mode) mutex locally
    std::lock_guard<std::recursive_mutex> guard(mutex_);

    if (change->kind == fastdds::rtps::ChangeKind_t::ALIVE)
    {
        EPROSIMA_LOG_WARNING(DISCOVERY_DATABASE,
                "Attempting to delete information of an ALIVE entity: " << guid_from_change(change));
        return false;
    }

    if (is_participant(change))
    {
        // The information related to this participant is cleaned up in process_data_queue()
        // when a disposal arrives, and it cleans also its children entities
        return delete_participant_entity_(guid_from_change(change).guidPrefix);
    }
    else if (is_reader(change))
    {
        // The information related to this reader is cleaned up in process_data_queue()
        return delete_reader_entity_(guid_from_change(change));
    }
    else if (is_writer(change))
    {
        // The information related to this writer is cleaned up in process_data_queue()
        return delete_writer_entity_(guid_from_change(change));
    }
    return false;
}

bool DiscoveryDataBase::data_queue_empty()
{
    return (pdp_data_queue_.BothEmpty() && edp_data_queue_.BothEmpty());
}

void DiscoveryDataBase::swap_data_queues()
{
    // Swap EDP before PDP to avoid race condition in which both data P and w/r are received at the same time,
    // just after having swapped the PDP queue
    edp_data_queue_.Swap();
    pdp_data_queue_.Swap();
}

bool DiscoveryDataBase::is_participant(
        const eprosima::fastdds::rtps::GUID_t& guid)
{
    return eprosima::fastdds::rtps::c_EntityId_RTPSParticipant == guid.entityId;
}

bool DiscoveryDataBase::is_writer(
        const eprosima::fastdds::rtps::GUID_t& guid)
{
    // RTPS Specification v2.3
    //    - For writers: NO_KEY = 0x03, WITH_KEY = 0x02
    //    - For built-in writers: NO_KEY = 0xc3, WITH_KEY = 0xc2
    // Furthermore, the Fast DDS Statistics Module defines an Entity ID for Statistics DataWriters
    const eprosima::fastdds::rtps::octet identifier = guid.entityId.value[3];
    return ((identifier == 0x02) ||
           (identifier == 0xc2) ||
           (identifier == 0x03) ||
           (identifier == 0xc3) ||
           eprosima::fastdds::statistics::is_statistics_builtin(guid.entityId));
}

bool DiscoveryDataBase::is_reader(
        const eprosima::fastdds::rtps::GUID_t& guid)
{
    // RTPS Specification v2.3
    // For readers: NO_KEY = 0x04, WITH_KEY = 0x07
    // For built-in readers: NO_KEY = 0xc4, WITH_KEY = 0xc7
    const eprosima::fastdds::rtps::octet identifier = guid.entityId.value[3];
    return ((identifier == 0x04) ||
           (identifier == 0xc4) ||
           (identifier == 0x07) ||
           (identifier == 0xc7));
}

bool DiscoveryDataBase::is_participant(
        const eprosima::fastdds::rtps::CacheChange_t* ch)
{
    return is_participant(guid_from_change(ch));
}

bool DiscoveryDataBase::is_writer(
        const eprosima::fastdds::rtps::CacheChange_t* ch)
{
    return is_writer(guid_from_change(ch));
}

bool DiscoveryDataBase::is_reader(
        const eprosima::fastdds::rtps::CacheChange_t* ch)
{
    return is_reader(guid_from_change(ch));
}

eprosima::fastdds::rtps::GUID_t DiscoveryDataBase::guid_from_change(
        const eprosima::fastdds::rtps::CacheChange_t* ch)
{
    return fastdds::rtps::iHandle2GUID(ch->instanceHandle);
}

fastdds::rtps::CacheChange_t* DiscoveryDataBase::cache_change_own_participant()
{
    auto part_it = participants_.find(server_guid_prefix_);
    if (part_it != participants_.end())
    {
        return part_it->second.change();
    }
    return nullptr;
}

const std::vector<fastdds::rtps::GuidPrefix_t> DiscoveryDataBase::direct_clients_and_servers()
{
    std::lock_guard<std::recursive_mutex> guard(mutex_);

    std::vector<fastdds::rtps::GuidPrefix_t> direct_clients_and_servers;
    // Iterate over participants to add the remote ones that are direct clients or servers
    for (auto participant: participants_)
    {
        // Only add participants other than the server
        if (server_guid_prefix_ != participant.first)
        {
            // Only add direct clients or server that are alive, not relayed ones.
            if (participant.second.is_local() && participant.second.change()->kind == eprosima::fastdds::rtps::ALIVE)
            {
                direct_clients_and_servers.push_back(participant.first);
            }
        }
    }
    return direct_clients_and_servers;
}

LocatorList DiscoveryDataBase::participant_metatraffic_locators(
        fastdds::rtps::GuidPrefix_t participant_guid_prefix)
{
    LocatorList locators;
    auto part_it = participants_.find(participant_guid_prefix);
    if (part_it != participants_.end())
    {
        for (auto locator : part_it->second.metatraffic_locators().unicast)
        {
            locators.push_back(locator);
        }
    }
    return locators;
}

DiscoveryDataBase::AckedFunctor DiscoveryDataBase::functor(
        eprosima::fastdds::rtps::CacheChange_t* change)
{
    return DiscoveryDataBase::AckedFunctor(this, change);
}

DiscoveryDataBase::AckedFunctor::AckedFunctor(
        DiscoveryDataBase* db,
        eprosima::fastdds::rtps::CacheChange_t* change)
    : db_(db)
    , change_(change)
    , pending_(false)
    // References its own state
    , external_pending_(pending_)
{
    // RAII only for the stateful object
    db_->lock_();
}

DiscoveryDataBase::AckedFunctor::AckedFunctor(
        const DiscoveryDataBase::AckedFunctor& r)
// References original state
    : external_pending_(r.external_pending_)
{
    db_ = r.db_;
    change_ = r.change_;
}

DiscoveryDataBase::AckedFunctor::~AckedFunctor()
{
    if (&external_pending_ == &pending_)
    {
        // Only the stateful object manages the lock
        db_->unlock_();
    }
}

void DiscoveryDataBase::AckedFunctor::operator () (
        const eprosima::fastdds::rtps::ReaderProxy* reader_proxy)
{
    std::lock_guard<std::recursive_mutex> guard(db_->mutex_);

    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "functor operator in change: " << change_->instanceHandle);
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "for reader proxy: " << reader_proxy->guid());
    // Check whether the change has been acknowledged by a given reader
    if (reader_proxy->rtps_is_relevant(change_))
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "is relevant, sequence number " << change_->sequenceNumber);
        if (reader_proxy->change_is_acked(change_->sequenceNumber))
        {
            // In the discovery database, mark the change as acknowledged by the reader
            db_->add_ack_(change_, reader_proxy->guid().guidPrefix);
        }
        else
        {
            // If the reader proxy is from a server that we are pinging, we may not want to wait
            // for it to be acked as the routine will not stop
            for (auto it = db_->servers_.begin(); it != db_->servers_.end(); ++it)
            {
                if (reader_proxy->guid().guidPrefix == *it)
                {
                    /*
                     * If the participant is already in the DB it means it has answered to the pinging
                     * or that is pinging us and we have already received its DATA(p)
                     * If neither has happenned (participant is not in DB)
                     * we should not wait for it to ack this data, or it could get stucked in an endless loop
                     * (this Remote Server could not exist and/or never be discovered)
                     * Nevertheless, the ack is still pending for this participant and once it is discovered this
                     * data will be sent again
                     */
                    auto remote_server_it = db_->participants_.find(*it);
                    if (remote_server_it == db_->participants_.end())
                    {
                        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Change " << change_->instanceHandle <<
                                "check as acked for " << reader_proxy->guid() << " as it has not answered pinging yet");
                        return;
                    }

                    break;
                }
            }

            // This change is relevant and has not been acked, and does not belongs to the reader proxy
            // of a server that has not been paired yet, so there are pending acknowledgements
            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Change " << change_->instanceHandle << " not acked yet");
            external_pending_ = true;
        }
    }
}

void DiscoveryDataBase::unmatch_participant_(
        const eprosima::fastdds::rtps::GuidPrefix_t& guid_prefix)
{
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "unmatching participant: " << guid_prefix);

    // For each participant remove it
    // IMPORTANT: This is not for every relevant participant, as participant A could be in other participant's B info
    // and B not be relevant for A. So it must be done for every Participant.
    for (auto& participant_it : participants_)
    {
        participant_it.second.remove_participant(guid_prefix);
    }
    for (auto& writer_it : writers_)
    {
        writer_it.second.remove_participant(guid_prefix);
    }
    for (auto& reader_it : readers_)
    {
        reader_it.second.remove_participant(guid_prefix);
    }
}

void DiscoveryDataBase::unmatch_writer_(
        const eprosima::fastdds::rtps::GUID_t& guid)
{
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "unmatching writer: " << guid);

    auto wit = writers_.find(guid);
    if (wit == writers_.end())
    {
        EPROSIMA_LOG_WARNING(DISCOVERY_DATABASE,
                "Attempting to unmatch an unexisting writer: " << guid);
        return;
    }

    // Get writer topic
    std::string topic = wit->second.topic();

    // Remove it from writer by topic
    remove_writer_from_topic_(guid, topic);

    // It there are more than one writer in this topic in the same participant we do not unmatch the endpoints
    if (!repeated_writer_topic_(guid.guidPrefix, topic))
    {
        // For each reader in same topic make not relevant. It could be none in readers
        auto tit = readers_by_topic_.find(topic);
        if (tit != readers_by_topic_.end())
        {
            for (auto reader : tit->second)
            {
                auto rit = readers_.find(reader);
                if (rit == readers_.end())
                {
                    EPROSIMA_LOG_WARNING(DISCOVERY_DATABASE,
                            "Unexisting reader " << reader << " in topic: " << topic);
                }
                else
                {
                    rit->second.remove_participant(guid.guidPrefix);
                }


            }
        }
    }
}

void DiscoveryDataBase::unmatch_reader_(
        const eprosima::fastdds::rtps::GUID_t& guid)
{
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "unmatching reader: " << guid);

    auto rit = readers_.find(guid);
    if (rit == readers_.end())
    {
        EPROSIMA_LOG_WARNING(DISCOVERY_DATABASE,
                "Attempting to unmatch an unexisting reader: " << guid);
        return;
    }

    // Get reader topic
    std::string topic = rit->second.topic();

    // Remove it from reader by topic
    remove_reader_from_topic_(guid, topic);

    // If there are more than one reader in this topic in the same participant we do not unmatch the endpoints
    if (!repeated_reader_topic_(guid.guidPrefix, topic))
    {
        // For each writer in same topic make not relevant. It could be none in writers
        auto tit = writers_by_topic_.find(topic);
        if (tit != writers_by_topic_.end())
        {
            for (auto writer : tit->second)
            {
                auto wit = writers_.find(writer);
                if (wit == writers_.end())
                {
                    EPROSIMA_LOG_WARNING(DISCOVERY_DATABASE,
                            "Unexisting writer " << writer << " in topic: " << topic);
                }
                else
                {
                    wit->second.remove_participant(guid.guidPrefix);
                }
            }
        }
    }
}

bool DiscoveryDataBase::repeated_writer_topic_(
        const eprosima::fastdds::rtps::GuidPrefix_t& participant,
        const std::string& topic_name)
{
    int count = 0;

    auto pit = participants_.find(participant);
    if (pit == participants_.end())
    {
        EPROSIMA_LOG_WARNING(DISCOVERY_DATABASE,
                "Checking repeated writer topics in an unexisting participant: " << participant);
        return false;
    }

    for (auto writer_guid : pit->second.writers())
    {
        auto wit = writers_.find(writer_guid);
        if (wit == writers_.end())
        {
            EPROSIMA_LOG_WARNING(DISCOVERY_DATABASE,
                    "writer missing: " << writer_guid);
        }

        if (wit->second.topic() == topic_name)
        {
            ++count;
            if (count > 1)
            {
                return true;
            }
        }
    }

    // We already know is false. Safety check
    return count > 1;
}

bool DiscoveryDataBase::repeated_reader_topic_(
        const eprosima::fastdds::rtps::GuidPrefix_t& participant,
        const std::string& topic_name)
{
    int count = 0;

    auto pit = participants_.find(participant);
    if (pit == participants_.end())
    {
        EPROSIMA_LOG_WARNING(DISCOVERY_DATABASE,
                "Checking repeated reader topics in an unexisting participant: " << participant);
        return false;
    }

    for (auto reader_guid : pit->second.readers())
    {
        auto rit = readers_.find(reader_guid);
        if (rit == readers_.end())
        {
            EPROSIMA_LOG_WARNING(DISCOVERY_DATABASE,
                    "reader missing: " << reader_guid);
            return false;
        }

        if (rit->second.topic() == topic_name)
        {
            ++count;
            if (count > 1)
            {
                return true;
            }
        }
    }

    // We already know is false. Safety check
    return count > 1;
}

void DiscoveryDataBase::remove_writer_from_topic_(
        const eprosima::fastdds::rtps::GUID_t& writer_guid,
        const std::string& topic_name)
{
    if (topic_name == virtual_topic_)
    {
        std::map<std::string, std::vector<eprosima::fastdds::rtps::GUID_t>>::iterator topic_it;
        for (topic_it = writers_by_topic_.begin(); topic_it != writers_by_topic_.end(); topic_it++)
        {
            for (std::vector<eprosima::fastdds::rtps::GUID_t>::iterator writer_it = topic_it->second.begin();
                    writer_it != topic_it->second.end();
                    ++writer_it)
            {
                if (*writer_it == writer_guid)
                {
                    topic_it->second.erase(writer_it);
                    break;
                }
            }
        }
    }
    else
    {
        std::map<std::string, std::vector<eprosima::fastdds::rtps::GUID_t>>::iterator topic_it =
                writers_by_topic_.find(topic_name);
        if (topic_it != writers_by_topic_.end())
        {
            for (std::vector<eprosima::fastdds::rtps::GUID_t>::iterator writer_it = topic_it->second.begin();
                    writer_it != topic_it->second.end();
                    ++writer_it)
            {
                if (*writer_it == writer_guid)
                {
                    topic_it->second.erase(writer_it);
                    break;
                }
            }
            // The topic wont be deleted to avoid creating and matching again all the virtual endpoints
            // This only affects a virtual endpoint, that will be added in this topic, but nothing will be matched
            // This also helps because topics are symetrical, and removing one implies check the other's emptyness first.
        }
    }


}

void DiscoveryDataBase::remove_reader_from_topic_(
        const eprosima::fastdds::rtps::GUID_t& reader_guid,
        const std::string& topic_name)
{
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "removing: " << reader_guid << " from topic " << topic_name);

    if (topic_name == virtual_topic_)
    {
        std::map<std::string, std::vector<eprosima::fastdds::rtps::GUID_t>>::iterator topic_it;
        for (topic_it = readers_by_topic_.begin(); topic_it != readers_by_topic_.end(); topic_it++)
        {
            for (std::vector<eprosima::fastdds::rtps::GUID_t>::iterator reader_it = topic_it->second.begin();
                    reader_it != topic_it->second.end();
                    ++reader_it)
            {
                if (*reader_it == reader_guid)
                {
                    topic_it->second.erase(reader_it);
                    break;
                }
            }
        }
    }
    else
    {
        std::map<std::string, std::vector<eprosima::fastdds::rtps::GUID_t>>::iterator topic_it =
                readers_by_topic_.find(topic_name);
        if (topic_it != readers_by_topic_.end())
        {
            for (std::vector<eprosima::fastdds::rtps::GUID_t>::iterator reader_it = topic_it->second.begin();
                    reader_it != topic_it->second.end();
                    ++reader_it)
            {
                if (*reader_it == reader_guid)
                {
                    topic_it->second.erase(reader_it);
                    break;
                }
            }
            // The topic wont be deleted to avoid creating and matching again all the virtual endpoints
            // this only affects a virtual endpoint, that will be added in this topic, but nothing will be matched
        }
    }
}

void DiscoveryDataBase::create_topic_(
        const std::string& topic_name)
{
    // Create writers topic
    auto wit = writers_by_topic_.insert(
        std::pair<std::string, std::vector<fastdds::rtps::GUID_t>>(
            topic_name,
            std::vector<fastdds::rtps::GUID_t>()));
    if (wit.second)
    {
        // Find virtual topic
        auto v_wit = writers_by_topic_.find(virtual_topic_);
        if (v_wit != writers_by_topic_.end())
        {
            // add all virtual writers
            // in case virtual topic does not exist do nothing
            for (fastdds::rtps::GUID_t virtual_writer : v_wit->second)
            {
                wit.first->second.push_back(virtual_writer);
            }
        }
    } // Else topic already existed

    // Create readers topic
    auto rit = readers_by_topic_.insert(
        std::pair<std::string, std::vector<fastdds::rtps::GUID_t>>(
            topic_name,
            std::vector<fastdds::rtps::GUID_t>()));
    if (rit.second)
    {
        // Find virtual topic
        auto v_rit = readers_by_topic_.find(virtual_topic_);
        if (v_rit != readers_by_topic_.end())
        {
            // add all virtual readers
            // in case virtual topic does not exist do nothing
            for (fastdds::rtps::GUID_t virtual_reader : v_rit->second)
            {
                rit.first->second.push_back(virtual_reader);
            }
        }
    } // Else topic already existed

    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "New topic " << topic_name << " created");
}

void DiscoveryDataBase::add_writer_to_topic_(
        const eprosima::fastdds::rtps::GUID_t& writer_guid,
        const std::string& topic_name)
{
    // Check if the topic exists already, if not create it
    auto it = writers_by_topic_.find(topic_name);
    if (it == writers_by_topic_.end())
    {
        create_topic_(topic_name);
        it = writers_by_topic_.find(topic_name);
    }

    // If the topic is virtual, add it in every topic, included virtual
    // could be recursive but it will call too many find functions
    if (topic_name == virtual_topic_)
    {
        for (auto it_topics = writers_by_topic_.begin();
                it_topics != writers_by_topic_.end();
                ++it_topics)
        {
            // This find should be useless because right now we only call this function from
            // create_writer_from_change, so the entity must be always new
            std::vector<eprosima::fastdds::rtps::GUID_t>::iterator writer_by_topic_it =
                    std::find(it_topics->second.begin(), it_topics->second.end(), writer_guid);
            if (writer_by_topic_it == it_topics->second.end())
            {
                EPROSIMA_LOG_INFO(DISCOVERY_DATABASE,
                        "New virtual writer " << writer_guid << " in writers_by_topic: " << it_topics->first);
                it_topics->second.push_back(writer_guid);
            }
        }
        // The writer has been already added to every topic, avoid try to add it again in virtual topic
        return;
    }

    // Add the writer in the topic
    std::vector<eprosima::fastdds::rtps::GUID_t>::iterator writer_by_topic_it =
            std::find(it->second.begin(), it->second.end(), writer_guid);
    if (writer_by_topic_it == it->second.end())
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "New writer " << writer_guid << " in writers_by_topic: " << topic_name);
        it->second.push_back(writer_guid);
    }
}

void DiscoveryDataBase::add_reader_to_topic_(
        const eprosima::fastdds::rtps::GUID_t& reader_guid,
        const std::string& topic_name)
{
    // Check if the topic exists already, if not create it
    auto it = readers_by_topic_.find(topic_name);
    if (it == readers_by_topic_.end())
    {
        create_topic_(topic_name);
        it = readers_by_topic_.find(topic_name);
    }

    // If the topic is virtual, add it in every topic, included virtual
    // could be recursive but it will call too many find functions
    if (topic_name == virtual_topic_)
    {
        for (auto it_topics = readers_by_topic_.begin();
                it_topics != readers_by_topic_.end();
                ++it_topics)
        {
            // This find should be useless because right now we only call this function from
            // create_reader_from_change, so the entity must be always new
            std::vector<eprosima::fastdds::rtps::GUID_t>::iterator reader_by_topic_it =
                    std::find(it_topics->second.begin(), it_topics->second.end(), reader_guid);
            if (reader_by_topic_it == it_topics->second.end())
            {
                EPROSIMA_LOG_INFO(DISCOVERY_DATABASE,
                        "New virtual reader " << reader_guid << " in readers_by_topic: " << it_topics->first);
                it_topics->second.push_back(reader_guid);
            }
        }
        // The reader has been already added to every topic, avoid try to add it again in virtual topic
        return;
    }

    // Add the reader in the topic
    std::vector<eprosima::fastdds::rtps::GUID_t>::iterator reader_by_topic_it =
            std::find(it->second.begin(), it->second.end(), reader_guid);
    if (reader_by_topic_it == it->second.end())
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "New reader " << reader_guid << " in readers_by_topic: " << topic_name);
        it->second.push_back(reader_guid);
    }
}

bool DiscoveryDataBase::delete_participant_entity_(
        const fastdds::rtps::GuidPrefix_t& guid_prefix)
{
    auto it = participants_.find(guid_prefix);
    if (it == participants_.end())
    {
        return false;
    }
    delete_participant_entity_(it);

    return true;
}

std::map<eprosima::fastdds::rtps::GuidPrefix_t, DiscoveryParticipantInfo>::iterator
DiscoveryDataBase::delete_participant_entity_(
        std::map<eprosima::fastdds::rtps::GuidPrefix_t, DiscoveryParticipantInfo>::iterator it)
{
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Deleting participant: " << it->first);
    if (it == participants_.end())
    {
        return participants_.end();
    }
    changes_to_release_.push_back(it->second.change());
    return participants_.erase(it);
}

bool DiscoveryDataBase::delete_reader_entity_(
        const fastdds::rtps::GUID_t& guid)
{
    // Find own reader
    auto it = readers_.find(guid);
    if (it == readers_.end())
    {
        return false;
    }

    delete_reader_entity_(it);
    return true;
}

std::map<eprosima::fastdds::rtps::GUID_t, DiscoveryEndpointInfo>::iterator DiscoveryDataBase::delete_reader_entity_(
        std::map<eprosima::fastdds::rtps::GUID_t, DiscoveryEndpointInfo>::iterator it)
{
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Deleting reader: " << it->first.guidPrefix);
    if (it == readers_.end())
    {
        return readers_.end();
    }
    // Remove entity from participant readers vector
    auto pit = participants_.find(it->first.guidPrefix);
    if (pit == participants_.end())
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Attempting to delete an orphan reader");
        // Returning error here could lead to an infinite loop
    }
    else
    {
        pit->second.remove_reader(it->first);
    }

    if (it->second.is_virtual())
    {
        // If the change is virtual, we can simply delete it
        delete it->second.change();
    }
    else
    {
        // Mark change to release
        changes_to_release_.push_back(it->second.change());
    }

    // Remove entity in readers_ map
    return readers_.erase(it);
}

bool DiscoveryDataBase::delete_writer_entity_(
        const fastdds::rtps::GUID_t& guid)
{
    // Find own writer
    auto it = writers_.find(guid);
    if (it == writers_.end())
    {
        return false;
    }

    delete_writer_entity_(it);
    return true;
}

std::map<eprosima::fastdds::rtps::GUID_t, DiscoveryEndpointInfo>::iterator DiscoveryDataBase::delete_writer_entity_(
        std::map<eprosima::fastdds::rtps::GUID_t, DiscoveryEndpointInfo>::iterator it)
{
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Deleting writer: " << it->first.guidPrefix);
    if (it == writers_.end())
    {
        return writers_.end();
    }
    // Remove entity from participant writers vector
    auto pit = participants_.find(it->first.guidPrefix);
    if (pit == participants_.end())
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Attempting to delete and orphan writer");
        // Returning error here could lead to an infinite loop
    }
    else
    {
        pit->second.remove_writer(it->first);
    }

    if (it->second.is_virtual())
    {
        // If the change is virtual, we can simply delete it
        delete it->second.change();
    }
    else
    {
        // Mark change to release
        changes_to_release_.push_back(it->second.change());
    }

    // Remove entity in writers_ map
    return writers_.erase(it);
}

bool DiscoveryDataBase::add_pdp_to_send_(
        eprosima::fastdds::rtps::CacheChange_t* change)
{
    // Add DATA(p) to send in next iteration if it is not already there
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (std::find(
                pdp_to_send_.begin(),
                pdp_to_send_.end(),
                change) == pdp_to_send_.end())
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Adding DATA(p) to send: "
                << change->instanceHandle);
        pdp_to_send_.push_back(change);
        return true;
    }
    return false;
}

bool DiscoveryDataBase::add_edp_publications_to_send_(
        eprosima::fastdds::rtps::CacheChange_t* change)
{
    // Add DATA(w) to send in next iteration if it is not already there
    if (std::find(
                edp_publications_to_send_.begin(),
                edp_publications_to_send_.end(),
                change) == edp_publications_to_send_.end())
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Adding DATA(w) to send: "
                << change->instanceHandle);
        edp_publications_to_send_.push_back(change);
        return true;
    }
    return false;
}

bool DiscoveryDataBase::add_edp_subscriptions_to_send_(
        eprosima::fastdds::rtps::CacheChange_t* change)
{
    // Add DATA(r) to send in next iteration if it is not already there
    if (std::find(
                edp_subscriptions_to_send_.begin(),
                edp_subscriptions_to_send_.end(),
                change) == edp_subscriptions_to_send_.end())
    {
        EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Adding DATA(r) to send: "
                << change->instanceHandle);
        edp_subscriptions_to_send_.push_back(change);
        return true;
    }
    return false;
}

void DiscoveryDataBase::to_json(
        nlohmann::json& j) const
{
    // The own server entities are not stored in the db, because in relaunch the must be created again
    // Participants
    auto pit = participants_.begin();
    j["participants"] = nlohmann::json({});
    while (pit != participants_.end())
    {
        if (pit->first != server_guid_prefix_)
        {
            nlohmann::json j_part;
            pit->second.to_json(j_part);
            j["participants"][ddb::object_to_string(pit->first)] = j_part;
        }
        ++pit;
    }

    // Writers
    auto wit = writers_.begin();
    j["writers"] = nlohmann::json({});
    while (wit != writers_.end())
    {
        if (wit->first.guidPrefix != server_guid_prefix_)
        {
            nlohmann::json j_w;
            wit->second.to_json(j_w);
            j["writers"][ddb::object_to_string(wit->first)] = j_w;
        }
        ++wit;
    }

    // Readers
    auto rit = readers_.begin();
    j["readers"] = nlohmann::json({});
    while (rit != readers_.end())
    {
        if (rit->first.guidPrefix != server_guid_prefix_)
        {
            nlohmann::json j_r;
            rit->second.to_json(j_r);
            j["readers"][ddb::object_to_string(rit->first)] = j_r;
        }
        ++rit;
    }

    // TODO add version
}

bool DiscoveryDataBase::from_json(
        nlohmann::json& j,
        std::map<eprosima::fastdds::rtps::InstanceHandle_t, fastdds::rtps::CacheChange_t*>& changes_map)
{
    // This function will parse each attribute in json backup, casting it to istringstream
    // std::istringstream(j[""]) >> obj;

    // Changes are taken from changes_map, with already created changes

    // Auxiliar variables to deserialize and create new objects of the ddb
    fastdds::rtps::InstanceHandle_t instance_handle_aux;
    fastdds::rtps::GuidPrefix_t prefix_aux;
    fastdds::rtps::GuidPrefix_t prefix_aux_ack;
    fastdds::rtps::GUID_t guid_aux;

    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Raising DDB from json Backup");

    try
    {
        // Participants
        for (auto it = j["participants"].begin(); it != j["participants"].end(); ++it)
        {
            // Populate info from participant to charge its change
            std::istringstream(it.key()) >> prefix_aux;
            std::istringstream(it.value()["change"]["instance_handle"].get<std::string>()) >> instance_handle_aux;

            // Get change
            fastdds::rtps::CacheChange_t* change;
            change = changes_map[instance_handle_aux];

            // Populate RemoteLocatorList
            fastdds::rtps::RemoteLocatorList rll;
            std::istringstream(it.value()["metatraffic_locators"].get<std::string>()) >> rll;

            // Populate DiscoveryParticipantChangeData
            DiscoveryParticipantChangeData dpcd(
                rll,
                it.value()["is_client"].get<bool>(),
                it.value()["is_local"].get<bool>());

            // Populate DiscoveryParticipantInfo
            DiscoveryParticipantInfo dpi(change, server_guid_prefix_, dpcd);

            // Add acks
            for (auto it_ack = it.value()["ack_status"].begin(); it_ack != it.value()["ack_status"].end(); ++it_ack)
            {
                // Populate GuidPrefix_t
                std::istringstream(it_ack.key()) >> prefix_aux_ack;

                dpi.add_or_update_ack_participant(prefix_aux_ack, it_ack.value().get<ParticipantState>());
            }

            // Add Participant
            participants_.insert(std::make_pair(prefix_aux, dpi));

            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Participant " << prefix_aux << " created");

            // In case the change is NOT ALIVE it must be set as dispose so it can be communicate to others and erased
            if (change->kind != fastdds::rtps::ALIVE)
            {
                disposals_.push_back(change);
            }
        }

        // Writers
        for (auto it = j["writers"].begin(); it != j["writers"].end(); ++it)
        {
            // Populate GUID_t
            std::istringstream(it.key()) >> guid_aux;
            std::istringstream(it.value()["change"]["instance_handle"].get<std::string>()) >> instance_handle_aux;

            // Get change
            fastdds::rtps::CacheChange_t* change;
            change = changes_map[instance_handle_aux];

            // Populate topic
            std::string topic = it.value()["topic"].get<std::string>();

            // Populate DiscoveryEndpointInfo
            DiscoveryEndpointInfo dei(change, topic, topic == virtual_topic_, server_guid_prefix_);

            // Add acks
            for (auto it_ack = it.value()["ack_status"].begin(); it_ack != it.value()["ack_status"].end(); ++it_ack)
            {
                // Populate GuidPrefix_t
                std::istringstream(it_ack.key()) >> prefix_aux_ack;

                dei.add_or_update_ack_participant(prefix_aux_ack, it_ack.value().get<ParticipantState>());
            }

            // Add Participant
            auto wit = writers_.insert(std::make_pair(guid_aux, dei));
            // wit is only used in log message below, so it's potentially unused.
            static_cast<void>(wit);

            // Extra configurations for writers
            // Add writer to writers_by_topic. This will create the topic if necessary
            add_writer_to_topic_(guid_aux, topic);

            // Add writer to its participant
            std::map<eprosima::fastdds::rtps::GuidPrefix_t, DiscoveryParticipantInfo>::iterator writer_part_it =
                    participants_.find(guid_aux.guidPrefix);
            if (writer_part_it != participants_.end())
            {
                writer_part_it->second.add_writer(guid_aux);
            }
            else
            {
                // Endpoint without participant, corrupted DDB
                EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "Writer " << guid_aux << " without participant");
                // TODO handle error
                return false;
            }

            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE,
                    "Writer " << guid_aux << " created with instance handle " <<
                    wit.first->second.change()->instanceHandle);

            if (change->kind != fastdds::rtps::ALIVE)
            {
                disposals_.push_back(change);
            }
        }

        // Readers
        for (auto it = j["readers"].begin(); it != j["readers"].end(); ++it)
        {
            // Populate GUID_t
            std::istringstream(it.key()) >> guid_aux;
            std::istringstream(it.value()["change"]["instance_handle"].get<std::string>()) >> instance_handle_aux;

            // Get change
            fastdds::rtps::CacheChange_t* change;
            change = changes_map[instance_handle_aux];

            // Populate topic
            std::string topic = it.value()["topic"].get<std::string>();

            // Populate DiscoveryEndpointInfo
            DiscoveryEndpointInfo dei(change, topic, topic == virtual_topic_, server_guid_prefix_);

            // Add acks
            for (auto it_ack = it.value()["ack_status"].begin(); it_ack != it.value()["ack_status"].end(); ++it_ack)
            {
                // Populate GuidPrefix_t
                std::istringstream(it_ack.key()) >> prefix_aux_ack;

                dei.add_or_update_ack_participant(prefix_aux_ack, it_ack.value().get<ParticipantState>());
            }

            // Add Participant
            readers_.insert(std::make_pair(guid_aux, dei));

            // Extra configurations for readers
            // Add reader to readers_by_topic. This will create the topic if necessary
            add_reader_to_topic_(guid_aux, topic);

            // Add reader to its participant
            std::map<eprosima::fastdds::rtps::GuidPrefix_t, DiscoveryParticipantInfo>::iterator reader_part_it =
                    participants_.find(guid_aux.guidPrefix);
            if (reader_part_it != participants_.end())
            {
                reader_part_it->second.add_reader(guid_aux);
            }
            else
            {
                // Endpoint without participant, corrupted DDB
                // TODO handle error
                return false;
            }
            EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Reader " << guid_aux << " created");

            if (change->kind != fastdds::rtps::ALIVE)
            {
                disposals_.push_back(change);
            }
        }
    }
    catch (std::ios_base::failure&)
    {
        EPROSIMA_LOG_ERROR(DISCOVERY_DATABASE, "BACKUP CORRUPTED");
    }

    // Set dirty topics to all, so next iteration every message pending is sent
    set_dirty_topic_(virtual_topic_);

    // Announce own server
    server_acked_by_all(false);

    return true;
}

void DiscoveryDataBase::clean_backup()
{
    EPROSIMA_LOG_INFO(DISCOVERY_DATABASE, "Restoring queue DDB in json backup");

    // This will erase the last backup stored
    backup_file_.close();
    backup_file_.open(backup_file_name_, std::ios_base::out);
}

void DiscoveryDataBase::persistence_enable(
        const std::string& backup_file_name)
{
    is_persistent_ = true;
    backup_file_name_ = backup_file_name;
    // It opens the file in append mode because the info in it has not been yet
    backup_file_.open(backup_file_name_, std::ios::app);
}

bool DiscoveryDataBase::is_participant_local(
        const eprosima::fastdds::rtps::GuidPrefix_t& participant_prefix)
{
    std::lock_guard<std::recursive_mutex> guard(mutex_);

    auto pit = participants_.find(participant_prefix);
    if (pit != participants_.end())
    {
        return pit->second.is_local();
    }
    return false; // In case it does not exist
}

} // namespace ddb
} // namespace rtps
} // namespace fastdds
} // namespace eprosima
