#pragma once

#include "lsm_lab/remote_merge.hpp"

namespace lsm_lab::detail
{
    std::unique_ptr<RpcClient> connect_tcp(const Endpoint &endpoint);
    std::unique_ptr<RpcServer> listen_tcp(const Endpoint &endpoint, RpcHandler handler);
#ifdef LSM_HAVE_RDMA
    std::unique_ptr<RpcClient> connect_rdma(const Endpoint &endpoint);
    std::unique_ptr<RpcServer> listen_rdma(const Endpoint &endpoint, RpcHandler handler);
#endif
}
