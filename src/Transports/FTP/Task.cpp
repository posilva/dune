//***************************************************************************
// Copyright 2007-2013 Universidade do Porto - Faculdade de Engenharia      *
// Laboratório de Sistemas e Tecnologia Subaquática (LSTS)                  *
//***************************************************************************
// This file is part of DUNE: Unified Navigation Environment.               *
//                                                                          *
// Commercial Licence Usage                                                 *
// Licencees holding valid commercial DUNE licences may use this file in    *
// accordance with the commercial licence agreement provided with the       *
// Software or, alternatively, in accordance with the terms contained in a  *
// written agreement between you and Universidade do Porto. For licensing   *
// terms, conditions, and further information contact lsts@fe.up.pt.        *
//                                                                          *
// European Union Public Licence - EUPL v.1.1 Usage                         *
// Alternatively, this file may be used under the terms of the EUPL,        *
// Version 1.1 only (the "Licence"), appearing in the file LICENCE.md       *
// included in the packaging of this file. You may not use this work        *
// except in compliance with the Licence. Unless required by applicable     *
// law or agreed to in writing, software distributed under the Licence is   *
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF     *
// ANY KIND, either express or implied. See the Licence for the specific    *
// language governing permissions and limitations at                        *
// https://www.lsts.pt/dune/licence.                                        *
//***************************************************************************
// Author: Ricardo Martins                                                  *
//***************************************************************************

// ISO C++ 98 headers.
#include <vector>
#include <list>

// DUNE headers.
#include <DUNE/DUNE.hpp>

// Local headers.
#include "Session.hpp"

namespace Transports
{
  namespace FTP
  {
    using DUNE_NAMESPACES;

    //! Task arguments
    struct Arguments
    {
      // Data port.
      uint16_t data_port;
      // Control port.
      uint16_t control_port;
    };

    struct Task: public Tasks::Task
    {
      //! Arguments
      Arguments m_args;
      //! Port bind retries.
      static const int c_port_retries = 5;
      //! Control sockets.
      std::list<TCPSocket*> m_sockets;
      //! IO selector.
      IOMultiplexing m_iom;
      //! List of busy sessions.
      std::list<Session*> m_busy_list;
      //! Concurrency lock for list of busy sessions.
      Mutex m_busy_list_lock;

      Task(const std::string& name, Tasks::Context& ctx):
        Tasks::Task(name, ctx)
      {
        param("Data Port", m_args.data_port)
        .defaultValue("20")
        .description("Data Port");

        param("Control Port", m_args.control_port)
        .defaultValue("21")
        .description("Control Port");
      }

      ~Task(void)
      {
        onResourceRelease();
      }

      TCPSocket*
      createSocket(Address addr, uint16_t port)
      {
        TCPSocket* sock = new TCPSocket;
        sock->bind(port, addr);
        sock->listen(5);
        inf(DTR("bound to port %s:%u"), addr.c_str(), port);
        return sock;
      }

      void
      onResourceAcquisition(void)
      {
        // Initialize and dispatch AnnounceService.
        std::vector<Interface> itfs = Interface::get();
        for (unsigned i = 0; i < itfs.size(); ++i)
        {
          std::stringstream os;
          os << "ftp://" << itfs[i].address().str() << ":" << m_args.control_port << "/";

          TCPSocket* sock = createSocket(itfs[i].address(), m_args.control_port);
          sock->addToPoll(m_iom);
          m_sockets.push_back(sock);

          IMC::AnnounceService announce;
          announce.service = os.str();

          if (itfs[i].address().isLoopback())
            announce.service_type = IMC::AnnounceService::SRV_TYPE_LOCAL;
          else
            announce.service_type = IMC::AnnounceService::SRV_TYPE_EXTERNAL;

          dispatch(announce);
        }
      }

      void
      onResourceRelease(void)
      {
        while (!m_busy_list.empty())
        {
          Session* session = m_busy_list.front();
          m_busy_list.pop_front();
          session->stopAndJoin();
          delete session;
        }

        while (!m_sockets.empty())
        {
          TCPSocket* socket = m_sockets.front();
          m_sockets.pop_front();
          delete socket;
        }
      }

      void
      acceptNewClient(TCPSocket* sock, const Address& local_addr)
      {
        try
        {
          Address addr;
          TCPSocket* client = sock->accept(&addr);

          debug("accepted connection from '%s'", addr.c_str());
          Session* handler = new Session(m_ctx, client, local_addr);
          handler->start();
          m_busy_list.push_back(handler);
        }
        catch (std::runtime_error& e)
        {
          err(DTR("error accepting new client connection: %s"), e.what());
        }
      }

      void
      cleanBusyList(void)
      {
        std::list<Session*>::iterator itr = m_busy_list.begin();
        for (; itr != m_busy_list.end(); ++itr)
        {
          if ((*itr)->isDead())
          {
            (*itr)->stopAndJoin();
            delete *itr;
            itr = m_busy_list.erase(itr);
          }
        }
      }

      void
      onMain(void)
      {
        while (!stopping())
        {
          if (!m_iom.poll(1.0))
            continue;

          std::list<TCPSocket*>::iterator itr = m_sockets.begin();
          for (; itr != m_sockets.end(); ++itr)
          {
            if ((*itr)->wasTriggered(m_iom))
              acceptNewClient(*itr, (*itr)->getBoundAddress());
          }

          cleanBusyList();
        }
      }
    };
  }
}

DUNE_TASK
