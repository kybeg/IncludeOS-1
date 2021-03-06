// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2015 Oslo and Akershus University College of Applied Sciences
// and Alfred Bratterud
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

#define DEBUG
#define DEBUG2

#include <os>
#include <net/tcp.hpp>
#include <net/util.hpp>
#include <net/packet.hpp>
#include <net/ip4/packet_tcp.hpp>

using namespace net;

TCP::TCP(Inet<LinkLayer,IP4>& inet)
  : inet_(inet), local_ip_(inet.ip_addr()), listeners()
{
  debug2("<TCP::TCP> Instantiating. Open ports: %i \n", listeners.size()); 
}


TCP::Socket& TCP::bind(Port p){
  auto listener = listeners.find(p);
  
  if (listener != listeners.end())
    panic("Port busy!");
  
  debug("<TCP bind> listening to port %i \n",p);
  // Create a socket and allow it to know about this stack
  
  listeners.emplace(p,TCP::Socket(inet_, p, TCP::Socket::CLOSED));

  listeners.at(p).listen(socket_backlog);
  
  debug("<TCP bind> Socket created and emplaced. State: %i\nThere are %i open ports. \n",
	listeners.at(p).poll(), listeners.size());
  return listeners.at(p);
}

TCP::Socket& TCP::connect(IP4::addr ip, Port p, net::TCP::Socket::connection_handler handler){
  debug("<TCP connect> Connecting to %s : %i \n", ip.str().c_str(), p);
  
  if (listeners.size() >= 0xfc00)
    panic("TCP Socket: All ports taken!");  

  debug("<TCP> finding free ephemeral port\n");  
  while (listeners.find(++current_ephemeral_) != listeners.end())
    if (current_ephemeral_  == 0) current_ephemeral_ = 1025; // prevent automatic ports under 1024
  
  debug("<TCP> Picked ephemeral port %i \n", current_ephemeral_);
  auto& sock = bind(current_ephemeral_);
  
  sock.onAccept(handler);
  
  sock.syn(ip, p);
  
  return sock;
  
}


uint16_t TCP::checksum(Packet_ptr pckt){  
  // Size has to be fetched from the frame
  
  full_header* hdr = (full_header*)pckt->buffer();
  
  IP4::ip_header* ip_hdr = &hdr->ip_hdr;
  tcp_header* tcp_hdr = &hdr->tcp_hdr;
  pseudo_header pseudo_hdr;
  
  int tcp_length_ = tcp_length(pckt);
  
  // Populate pseudo header
  pseudo_hdr.saddr.whole = ip_hdr->saddr.whole;
  pseudo_hdr.daddr.whole = ip_hdr->daddr.whole;
  pseudo_hdr.zero = 0;
  pseudo_hdr.proto = IP4::IP4_TCP;	 
  pseudo_hdr.tcp_length = htons(tcp_length_);
    
  union {
    uint32_t whole;
    uint16_t part[2];
  }sum;
  
  sum.whole = 0;
    
  // Compute sum of pseudo header
  for (uint16_t* it = (uint16_t*)&pseudo_hdr; it < (uint16_t*)&pseudo_hdr + sizeof(pseudo_hdr)/2; it++)
    sum.whole += *it;       
  
  // Compute sum sum the actual header and data
  for (uint16_t* it = (uint16_t*)tcp_hdr; it < (uint16_t*)tcp_hdr + tcp_length_/2; it++)
    sum.whole+= *it;

  // The odd-numbered case
  if (tcp_length_ & 1) {
    debug("<TCP::checksum> ODD number of bytes. 0-pading \n");
    union {
      uint16_t whole;
      uint8_t part[2];
    }last_chunk;
    last_chunk.part[0] = ((uint8_t*)tcp_hdr)[tcp_length_ - 1];
    last_chunk.part[1] = 0;
    sum.whole += last_chunk.whole;
  }
  
  debug2("<TCP::checksum: sum: 0x%x, half+half: 0x%x, TCP checksum: 0x%x, TCP checksum big-endian: 0x%x \n",
	 sum.whole, sum.part[0] + sum.part[1], (uint16_t)~((uint16_t)(sum.part[0] + sum.part[1])), htons((uint16_t)~((uint16_t)(sum.part[0] + sum.part[1]))));
  
  return ~(sum.part[0] + sum.part[1]);
  
}


int TCP::transmit(Packet_ptr pckt){
  
  auto tcp_pckt = std::static_pointer_cast<PacketIP4> (pckt);
  
  //pckt->init();  
  
  full_header* full_hdr = (full_header*)pckt->buffer();
  tcp_header* hdr = &(full_hdr->tcp_hdr);
  
  // Set source address
  full_hdr->ip_hdr.saddr.whole = local_ip_.whole;
  
  hdr->set_offset(5);
  hdr->checksum = 0;  
  hdr->checksum = checksum(pckt);
  return _network_layer_out(pckt);
};


int TCP::bottom(Packet_ptr pckt){
  
  auto pckt4 = std::static_pointer_cast<TCP_packet>(pckt);
  debug("<TCP::bottom> Upstream TCP-packet received, to TCP @ %p \n", this);
  debug("<TCP::bottom> There are %i open ports \n", listeners.size());
  
  if (checksum(pckt))
    debug("<TCP::bottom> WARNING: Incoming TCP Packet checksum is not 0. Continuing.\n");
  
  debug("<TCP::bottom> Incoming packet TCP-checksum: 0x%x \n", pckt4->gen_checksum());
  
  debug("<TCP::bottom> Destination port: %i, Source port: %i \n", 
	pckt4->dst_port(), pckt4->src_port());
  
  auto listener = listeners.find(pckt4->dst_port());
  
  if (listener == listeners.end()){
    debug("<TCP::bottom> Nobody's listening to this port. Ignoring");
    debug("<TCP::bottom> There are %i open ports \n", listeners.size());
    return 0;
  }
  
  debug("<TCP::bottom> Somebody's listening to this port. State: %i. Passing it up to the socket \n",listener->second.poll());
  
  // If reset flag is set, delete the connection.
  // @todo publish an onClose-event here
  if (pckt4->isset(TCP::RST)) {
    debug("<TCP::bottom> But it's a RESET-packet, so closing \n");
    listeners.erase(listener);
    return 0;    
  }
          
  // Pass the packet up to the listening socket
  (*listener).second.bottom(pckt);
  
  
  return 0;
  
}
