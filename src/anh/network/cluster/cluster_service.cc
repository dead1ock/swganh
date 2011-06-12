/*
---------------------------------------------------------------------------------------
This source file is part of SWG:ANH (Star Wars Galaxies - A New Hope - Server Emulator)

For more information, visit http://www.swganh.com

Copyright (c) 2006 - 2010 The SWG:ANH Team
---------------------------------------------------------------------------------------
Use of this source code is governed by the GPL v3 license that can be found
in the COPYING file or at http://www.gnu.org/licenses/gpl-3.0.html

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
---------------------------------------------------------------------------------------
*/
#pragma warning (disable : 4355)

#include <anh/network/cluster/cluster_service.h>

#include <anh/network/cluster/tcp_message.h>
#include <anh/event_dispatcher/event_dispatcher.h>
#include <anh/server_directory/server_directory_events.h>
#include <packets/NetworkEventMessage.h>
#include <iostream>
#include <anh/byte_buffer.h>

#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>

#include <glog/logging.h>

using namespace anh::server_directory;
using namespace anh::event_dispatcher;
using namespace std;

namespace anh {
namespace network {
namespace cluster {
ClusterService::ClusterService(boost::asio::io_service& io_service, shared_ptr<ServerDirectoryInterface> directory,
    std::shared_ptr<anh::event_dispatcher::EventDispatcherInterface> dispatcher, uint16_t port)
    : directory_(directory)
    , event_dispatcher_(dispatcher)
    , io_service_(io_service)
    , resolver_(io_service)
    , send_packet_filter_(this)
    , receive_packet_filter_(this)
    , outgoing_start_filter_(this)
    , packet_event_filter_(this)
    , port_(port)
{
    outgoing_pipeline_.add_filter(outgoing_start_filter_);
    outgoing_pipeline_.add_filter(send_packet_filter_);
    incoming_pipeline_.add_filter(receive_packet_filter_);
    incoming_pipeline_.add_filter(packet_event_filter_);

    // setup our listener to add process on event
    auto register_service_listener = [=] (shared_ptr<EventInterface> incoming_event)->bool {
        auto add_service = static_pointer_cast<BasicEvent<ProcessData>>(incoming_event);
        Connect(std::make_shared<Process>(std::move(add_service->process)));
        return true;
    };

    // setup listener to remove process on event
    auto remove_process_listener = [=] (shared_ptr<EventInterface> incoming_event)->bool {
        auto remove_service = static_pointer_cast<BasicEvent<ProcessData>>(incoming_event);
        Disconnect(std::make_shared<Process>(std::move(remove_service->process)));
        return true;
    };

    event_dispatcher_->subscribe("RegisterProcess", register_service_listener);
    event_dispatcher_->subscribe("RemoveProcess", remove_process_listener);
}

ClusterService::~ClusterService(void)
{
    // interrupt/join thread
    service_thread_.interrupt();
    service_thread_.join();
}

void ClusterService::Start(std::shared_ptr<tcp_host> host)
{
    acceptor_ = std::make_shared<boost::asio::ip::tcp::acceptor>(io_service_, tcp::endpoint(tcp::v4(), port_ ));
    // if after assigning the host we have a nullptr, lets instantiate it
    if ((tcp_host_ = host) == nullptr)
    {
        tcp_host_ = std::make_shared<tcp_host>(io_service_, std::bind(&ClusterService::OnTCPHostReceive_, this, std::placeholders::_1));
    }

    acceptor_->async_accept(tcp_host_->socket(), boost::bind(&ClusterService::handle_accept_, this,
       tcp_host_ , boost::asio::placeholders::error));
    DLOG(WARNING) << "Now listening for TCP Messages on: " << port_;
    service_thread_ = boost::thread([=] () {
        io_service_.run();
    });
}

void ClusterService::Update(void)
{
    incoming_pipeline_.run(100);
    outgoing_pipeline_.run(100);
 }

void ClusterService::Shutdown(void)
{
    tcp_client_map_.clear();
    incoming_messages_.clear();
    outgoing_messages_.clear();
    incoming_pipeline_.clear();
    outgoing_pipeline_.clear();
}

bool ClusterService::isConnected(const std::string& name)
{
    auto find_it = std::find_if(tcp_client_map_.begin(), tcp_client_map_.end(), [name] (ClusterPair pair) {
        return (pair.first->name() == name);
    });
    return find_it != tcp_client_map_.end();
}
bool ClusterService::isConnected(std::shared_ptr<Process> process)
{
    auto find_it = std::find_if(tcp_client_map_.begin(), tcp_client_map_.end(), [process] (ClusterPair pair) {
        return process == pair.first;
    });

    return find_it != tcp_client_map_.end();
}
void ClusterService::Connect(std::shared_ptr<anh::server_directory::Process> process)
{
    try
    {
        // make sure we aren't already connected and this isn't us.
        if (!isConnected(process) && (process->tcp_port() != port_ ))
        {
            auto client = std::make_shared<tcp_client>(io_service_, process->address(), process->tcp_port());
            if (client != nullptr) {
                tcp_client_map_.insert(ClusterPair(process, client));
            }
        }
    }
    catch (exception e)
    {
        LOG(WARNING) << "Exception in Service::Connect " << e.what() << std::endl;
    }
}
void ClusterService::Connect(std::shared_ptr<tcp_client> client, std::shared_ptr<anh::server_directory::Process> process)
{
    try
    {
        // make sure we don't already have this client in our map
        if (!isConnected(process) && (process->tcp_port() != port_ ))
        {
            if (client != nullptr) {
                tcp_client_map_.insert(ClusterPair(process, client));
            }
        }
    }
    catch (exception e)
    {
        LOG(WARNING) << "Exception in Service::Connect " << e.what() << std::endl;
    }
}
void ClusterService::Disconnect(std::shared_ptr<anh::server_directory::Process> process)
{
    try
    {
        // make sure we are already connected
        if (isConnected(process))
        {
            // remove if the process was found
            auto find_it = std::find_if(tcp_client_map_.begin(), tcp_client_map_.end(), [=] (ClusterPair pair) {
                return (process == pair.first);
            });
            tcp_client_map_.erase(find_it);
        }
    }
    catch (exception e)
    {
        LOG(WARNING) << "Exception in Service::Disconnect " << e.what() << std::endl;
    }
}
void ClusterService::sendMessage(const std::string& name, std::shared_ptr<anh::event_dispatcher::EventInterface> event_out, DestinationType dest)
{
    // make sure we have a connection
    auto conn = getConnection(name);
    if (conn != nullptr)
    {
        anh::ByteBuffer buffer;
        event_out->serialize(buffer);
        auto tcp_message = new TCPMessage(conn, make_shared<anh::ByteBuffer>(buffer), dest);
        outgoing_messages_.push_back(tcp_message);
    }
    else
    {
        DLOG(FATAL) << "No Connection Established, could not send message to: " << name << endl;
    }
}
void ClusterService::sendMessage(const std::string& host, uint16_t port, anh::ByteBuffer& buffer, DestinationType dest)
{
    // check for a connection
    auto conn = getConnection(host, port);
    if (conn != nullptr)
    {
        auto bufferz = std::make_shared<anh::ByteBuffer>(std::move(buffer));
        auto tcp_message = new TCPMessage(conn, bufferz, dest);
        outgoing_messages_.push_back(tcp_message);
    }
}
void ClusterService::sendMessageByType_(const std::string& type, anh::ByteBuffer& buffer)
{
    // get all connections by type
    std::for_each(tcp_client_map_.begin(), tcp_client_map_.end(), [=, &buffer] (ClusterPair pair) {
        if (pair.first->type() == type)
        {
            // send message to each
            pair.second->Send(buffer);
        }
    });
}
void ClusterService::sendMessageToAll_(anh::ByteBuffer& buffer)
{
    std::for_each(tcp_client_map_.begin(), tcp_client_map_.end(), [=, &buffer] (ClusterPair pair) {
            // send message to each
            pair.second->Send(buffer);
    });
}
std::shared_ptr<tcp_client> ClusterService::getConnection(const std::string& host, uint16_t port)
{
    auto conn_it = std::find_if(tcp_client_map_.begin(), tcp_client_map_.end(), [host, port] (ClusterPair pair) {
        return (pair.first->address() == host && pair.first->tcp_port() == port);
    });
    if (conn_it != tcp_client_map_.end())
    {
        return (*conn_it).second;
    }
    return nullptr;
}
std::shared_ptr<tcp_client> ClusterService::getConnection(const std::string& name)
{
    auto conn_it = std::find_if(tcp_client_map_.begin(), tcp_client_map_.end(), [name] (ClusterPair pair) {
        return (pair.first->name() == name);
    });
    if (conn_it != tcp_client_map_.end())
    {
        return (*conn_it).second;
    }
    return nullptr;
}

void ClusterService::handle_accept_(std::shared_ptr<tcp_host> host, const boost::system::error_code& error)
{
    if (!error)
    {
        host->Start();
        host = std::make_shared<tcp_host>(io_service_, std::bind(&ClusterService::OnTCPHostReceive_, this, std::placeholders::_1));
        acceptor_->async_accept(host->socket(),
            std::bind(&ClusterService::handle_accept_, this, host,
            std::placeholders::_1));
    }
    else
    {
        LOG(WARNING) << "Error in Service::handle_accept: " << error.message() << std::endl;
    }
}

void ClusterService::OnTCPHostReceive_(anh::ByteBuffer* buffer)
{
    // add to the pipeline list
    incoming_messages_.push_back(buffer);
}
} // namespace soe
} // namespace network
} // namespace anh

#pragma warning (default : 4355)