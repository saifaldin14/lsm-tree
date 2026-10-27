#include "transport.hpp"

#include <stdexcept>

namespace lsm_lab
{
    bool rdma_supported()
    {
#ifdef LSM_HAVE_RDMA
        return true;
#else
        return false;
#endif
    }

    std::unique_ptr<RpcClient> connect_rpc(const Endpoint &endpoint)
    {
        if (endpoint.scheme == "tcp")
            return detail::connect_tcp(endpoint);
#ifdef LSM_HAVE_RDMA
        if (endpoint.scheme == "rdma")
            return detail::connect_rdma(endpoint);
#endif
        throw std::invalid_argument("transport not built: " + endpoint.text());
    }

    std::unique_ptr<RpcServer> listen_rpc(const Endpoint &endpoint, RpcHandler handler)
    {
        if (endpoint.scheme == "tcp")
            return detail::listen_tcp(endpoint, std::move(handler));
#ifdef LSM_HAVE_RDMA
        if (endpoint.scheme == "rdma")
            return detail::listen_rdma(endpoint, std::move(handler));
#endif
        throw std::invalid_argument("transport not built: " + endpoint.text());
    }
}
