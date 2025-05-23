// Copyright 2025 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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
 * @file RpcTimeoutException.hpp
 */

#ifndef FASTDDS_DDS_RPC_EXCEPTIONS__RPCTIMEOUTEXCEPTION_HPP
#define FASTDDS_DDS_RPC_EXCEPTIONS__RPCTIMEOUTEXCEPTION_HPP

#include <fastdds/fastdds_dll.hpp>
#include <fastdds/dds/rpc/exceptions/RpcException.hpp>

namespace eprosima {
namespace fastdds {
namespace dds {
namespace rpc {

/**
 * Exception thrown by the RPC API when an operation times out.
 */
class RpcTimeoutException : public RpcException
{

public:

    /**
     * Constructor.
     */
    RpcTimeoutException()
        : RpcException("The RPC operation timed out")
    {
    }

    /**
     * Copy constructor.
     */
    RpcTimeoutException(
            const RpcTimeoutException& other) noexcept = default;

    /**
     * Copy assignment.
     */
    RpcTimeoutException& operator =(
            const RpcTimeoutException& other) noexcept = default;

    /**
     * Destructor.
     */
    virtual ~RpcTimeoutException() noexcept = default;

};

}  // namespace rpc
}  // namespace dds
}  // namespace fastdds
}  // namespace eprosima

#endif  // FASTDDS_DDS_RPC_EXCEPTIONS__RPCTIMEOUTEXCEPTION_HPP
