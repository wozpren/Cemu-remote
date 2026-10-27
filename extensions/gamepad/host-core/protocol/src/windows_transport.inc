// SPDX-License-Identifier: GPL-3.0-only
// Sunshine Windows direct UDP sender; modified 2026-10-27 for Cemu Remote GamePad.
  SOCKADDR_IN to_sockaddr(boost::asio::ip::address_v4 address, uint16_t port) {
    SOCKADDR_IN saddr_v4 = {};

    saddr_v4.sin_family = AF_INET;
    saddr_v4.sin_port = htons(port);

    auto addr_bytes = address.to_bytes();
    memcpy(&saddr_v4.sin_addr, addr_bytes.data(), sizeof(saddr_v4.sin_addr));

    return saddr_v4;
  }

  SOCKADDR_IN6 to_sockaddr(boost::asio::ip::address_v6 address, uint16_t port) {
    SOCKADDR_IN6 saddr_v6 = {};

    saddr_v6.sin6_family = AF_INET6;
    saddr_v6.sin6_port = htons(port);
    saddr_v6.sin6_scope_id = address.scope_id();

    auto addr_bytes = address.to_bytes();
    memcpy(&saddr_v6.sin6_addr, addr_bytes.data(), sizeof(saddr_v6.sin6_addr));

    return saddr_v6;
  }

  /**
   * @brief Write the source IP address into the first ancillary control message.
   *
   * @param msg Socket message with control storage sized for packet information.
   * @param source_address Local IP address to use for outgoing packets.
   * @return Bytes occupied by the packet-information control message.
   */
  static ULONG populate_source_pktinfo(WSAMSG &msg, const boost::asio::ip::address &source_address) {
    ULONG cmbuflen = 0;
    auto cm = WSA_CMSG_FIRSTHDR(&msg);
    if (source_address.is_v6()) {
      IN6_PKTINFO pktInfo;

      SOCKADDR_IN6 saddr_v6 = to_sockaddr(source_address.to_v6(), 0);
      pktInfo.ipi6_addr = saddr_v6.sin6_addr;
      pktInfo.ipi6_ifindex = 0;

      cmbuflen += WSA_CMSG_SPACE(sizeof(pktInfo));

      cm->cmsg_level = IPPROTO_IPV6;
      cm->cmsg_type = IPV6_PKTINFO;
      cm->cmsg_len = WSA_CMSG_LEN(sizeof(pktInfo));
      memcpy(WSA_CMSG_DATA(cm), &pktInfo, sizeof(pktInfo));
    } else {
      IN_PKTINFO pktInfo;

      SOCKADDR_IN saddr_v4 = to_sockaddr(source_address.to_v4(), 0);
      pktInfo.ipi_addr = saddr_v4.sin_addr;
      pktInfo.ipi_ifindex = 0;

      cmbuflen += WSA_CMSG_SPACE(sizeof(pktInfo));

      cm->cmsg_level = IPPROTO_IP;
      cm->cmsg_type = IP_PKTINFO;
      cm->cmsg_len = WSA_CMSG_LEN(sizeof(pktInfo));
      memcpy(WSA_CMSG_DATA(cm), &pktInfo, sizeof(pktInfo));
    }

    return cmbuflen;
  }

  // Use UDP segmentation offload if it is supported by the OS. If the NIC is capable, this will use
  // hardware acceleration to reduce CPU usage. Support for USO was introduced in Windows 10 20H1.
  bool send_batch(const batched_send_info_t &send_info) {
    WSAMSG msg;

    // Convert the target address into a SOCKADDR
    SOCKADDR_IN taddr_v4;
    SOCKADDR_IN6 taddr_v6;
    if (send_info.target_address.is_v6()) {
      taddr_v6 = to_sockaddr(send_info.target_address.to_v6(), send_info.target_port);

      msg.name = (PSOCKADDR) &taddr_v6;
      msg.namelen = sizeof(taddr_v6);
    } else {
      taddr_v4 = to_sockaddr(send_info.target_address.to_v4(), send_info.target_port);

      msg.name = (PSOCKADDR) &taddr_v4;
      msg.namelen = sizeof(taddr_v4);
    }

    auto const max_bufs_per_msg = send_info.payload_buffers.size() + (send_info.headers ? 1 : 0);

    std::vector<WSABUF> bufs((send_info.headers ? send_info.block_count : 1) * max_bufs_per_msg);
    DWORD bufcount = 0;
    if (send_info.headers) {
      // Interleave buffers for headers and payloads
      for (auto i = 0; i < send_info.block_count; i++) {
        bufs[bufcount].buf = (char *) &send_info.headers[(send_info.block_offset + i) * send_info.header_size];
        bufs[bufcount].len = send_info.header_size;
        bufcount++;
        auto payload_desc = send_info.buffer_for_payload_offset((send_info.block_offset + i) * send_info.payload_size);
        bufs[bufcount].buf = (char *) payload_desc.buffer;
        bufs[bufcount].len = send_info.payload_size;
        bufcount++;
      }
    } else {
      // Translate buffer descriptors into WSABUFs
      auto payload_offset = send_info.block_offset * send_info.payload_size;
      auto payload_length = payload_offset + (send_info.block_count * send_info.payload_size);
      while (payload_offset < payload_length) {
        auto payload_desc = send_info.buffer_for_payload_offset(payload_offset);
        bufs[bufcount].buf = (char *) payload_desc.buffer;
        bufs[bufcount].len = std::min(payload_desc.size, payload_length - payload_offset);
        payload_offset += bufs[bufcount].len;
        bufcount++;
      }
    }

    msg.lpBuffers = bufs.data();
    msg.dwBufferCount = bufcount;
    msg.dwFlags = 0;

    // At most, one DWORD option and one PKTINFO option
    std::array<char, WSA_CMSG_SPACE(sizeof(DWORD)) + std::max(WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)), WSA_CMSG_SPACE(sizeof(IN_PKTINFO)))> cmbuf = {};
    msg.Control.buf = cmbuf.data();
    msg.Control.len = cmbuf.size();

    auto cmbuflen = populate_source_pktinfo(msg, send_info.source_address);

    if (send_info.block_count > 1) {
      cmbuflen += WSA_CMSG_SPACE(sizeof(DWORD));

      auto cm = WSA_CMSG_NXTHDR(&msg, WSA_CMSG_FIRSTHDR(&msg));
      cm->cmsg_level = IPPROTO_UDP;
      cm->cmsg_type = UDP_SEND_MSG_SIZE;
      cm->cmsg_len = WSA_CMSG_LEN(sizeof(DWORD));
      *((DWORD *) WSA_CMSG_DATA(cm)) = send_info.header_size + send_info.payload_size;
    }

    msg.Control.len = cmbuflen;

    // If USO is not supported, this will fail and the caller will fall back to unbatched sends.
    DWORD bytes_sent;
    bool success = WSASendMsg((SOCKET) send_info.native_socket, &msg, 0, &bytes_sent, nullptr, nullptr) != SOCKET_ERROR;
    if (success) cemu_host::packet_submitted(true);
    return success;
  }

  bool send(send_info_t &send_info) {
    WSAMSG msg;

    // Convert the target address into a SOCKADDR
    SOCKADDR_IN taddr_v4;
    SOCKADDR_IN6 taddr_v6;
    if (send_info.target_address.is_v6()) {
      taddr_v6 = to_sockaddr(send_info.target_address.to_v6(), send_info.target_port);

      msg.name = (PSOCKADDR) &taddr_v6;
      msg.namelen = sizeof(taddr_v6);
    } else {
      taddr_v4 = to_sockaddr(send_info.target_address.to_v4(), send_info.target_port);

      msg.name = (PSOCKADDR) &taddr_v4;
      msg.namelen = sizeof(taddr_v4);
    }

    std::array<WSABUF, 2> bufs;
    DWORD bufcount = 0;
    if (send_info.header) {
      bufs[bufcount].buf = (char *) send_info.header;
      bufs[bufcount].len = send_info.header_size;
      bufcount++;
    }
    bufs[bufcount].buf = (char *) send_info.payload;
    bufs[bufcount].len = send_info.payload_size;
    bufcount++;

    msg.lpBuffers = bufs.data();
    msg.dwBufferCount = bufcount;
    msg.dwFlags = 0;

    std::array<char, std::max(WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)), WSA_CMSG_SPACE(sizeof(IN_PKTINFO)))> cmbuf = {};
    msg.Control.buf = cmbuf.data();
    msg.Control.len = cmbuf.size();

    auto cmbuflen = populate_source_pktinfo(msg, send_info.source_address);

    msg.Control.len = cmbuflen;

    DWORD bytes_sent;
    if (WSASendMsg((SOCKET) send_info.native_socket, &msg, 0, &bytes_sent, nullptr, nullptr) == SOCKET_ERROR) {
      auto winerr = WSAGetLastError();
      BOOST_LOG(warning) << "WSASendMsg() failed: "sv << winerr;
      cemu_host::packet_submitted(false);
      return false;
    }

    cemu_host::packet_submitted(true);
    return true;
  }

