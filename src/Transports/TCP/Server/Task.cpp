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
// Author: Eduardo Marques                                                  *
//***************************************************************************

// DUNE headers.
#include <DUNE/DUNE.hpp>

namespace Transports
{
  namespace TCP
  {
    namespace Server
    {
      using DUNE_NAMESPACES;

      //! Task arguments
      struct Arguments
      {
        //! Port to use.
        uint16_t port;
        //! True to announce service.
        bool announce;
      };

      struct Task: public Tasks::SimpleTransport
      {
        // Arguments
        Arguments m_args;
        // Port bind retries.
        static const int c_port_retries = 5;
        // Server socket handle.
        TCPSocket* m_sock;
        // IO selector.
        IOMultiplexing m_iom;

        // Client data.
        struct Client
        {
          TCPSocket* socket; // Socket handle.
          Address address; // Client address.
          uint16_t port; // Client port.
          IMC::Parser parser; // Parser handle
        };

        // Client list.
        typedef std::list<Client> ClientList;
        ClientList m_clients;

        Task(const std::string& name, Tasks::Context& ctx):
          Tasks::SimpleTransport(name, ctx),
          m_sock(0)
        {
          param("Port", m_args.port)
          .defaultValue("7001")
          .description("TCP server port");

          param("Announce Service", m_args.announce)
          .defaultValue("true")
          .description("Set to true to announce the service");
        }

        ~Task(void)
        {
          onResourceRelease();
        }

        void
        onResourceAcquisition(void)
        {
          int port_limit = m_args.port + c_port_retries;

          m_sock = new TCPSocket;

          while (m_args.port != port_limit)
          {
            try
            {
              m_sock->bind(m_args.port);
              break;
            }
            catch (std::runtime_error& e)
            {
              war(DTR("failed to bind to port %u: %s"), m_args.port, e.what());
              ++m_args.port;
            }
          }

          if (m_args.port == port_limit)
          {
            std::string str = DTR("could not bind server socket");
            err("%s", str.c_str());
            throw std::runtime_error(str);
          }

          m_sock->listen(5);
          m_sock->addToPoll(m_iom);
          inf(DTR("bound to port %u"), m_args.port);

          if (m_args.announce)
          {
            // Initialize and dispatch AnnounceService.
            std::vector<Interface> itfs = Interface::get();

            for (unsigned i = 0; i < itfs.size(); ++i)
            {
              std::stringstream os;
              os << "imc+tcp://" << itfs[i].address().str() << ":" << m_args.port << "/";

              IMC::AnnounceService announce;
              announce.service = os.str();

              if (itfs[i].address().isLoopback())
                announce.service_type = IMC::AnnounceService::SRV_TYPE_LOCAL;
              else
                announce.service_type = IMC::AnnounceService::SRV_TYPE_EXTERNAL;

              dispatch(announce);
            }
          }
        }

        void
        closeConnection(Client& c, std::exception& e)
        {
          inf(DTR("closing connection to %s:%u (%s), client count is %lu"),
              c.address.c_str(), c.port, e.what(), (long unsigned int)(m_clients.size() - 1));

          c.socket->delFromPoll(m_iom);
          delete c.socket;
        }

        void
        onResourceRelease(void)
        {
          for (ClientList::iterator itr = m_clients.begin(); itr != m_clients.end(); ++itr)
          {
            itr->socket->delFromPoll(m_iom);
            delete itr->socket;
          }

          m_clients.clear();

          if (m_sock)
          {
            m_sock->delFromPoll(m_iom);
            delete m_sock;
            m_sock = 0;
          }
        }

        void
        onDataTransmission(const uint8_t* p, unsigned int n)
        {
          ClientList::iterator itr = m_clients.begin();

          while (itr != m_clients.end())
          {
            try
            {
              itr->socket->write((char*)p, n);
            }
            catch (std::runtime_error& e)
            {
              closeConnection(*itr, e);
              itr = m_clients.erase(itr);
              continue;
            }
            ++itr;
          }
        }

        void
        onDataReception(uint8_t* buf, unsigned int cap, double timeout)
        {
          // Poll for connections and client data
          if (!m_iom.poll(timeout))
            return;

          // Check for new clients.
          if (m_sock->wasTriggered(m_iom))
            acceptNewClient();

          // Check for client data
          handleClients(buf, cap);
        }

        void
        acceptNewClient(void)
        {
          Client c;
          c.socket = 0;
          try
          {
            c.socket = m_sock->accept(&c.address, &c.port);
            c.socket->addToPoll(m_iom);
            m_clients.push_back(c);
            inf(DTR("accepted connection from %s:%u, client count is %lu"),
                c.address.c_str(), c.port, (long unsigned int)m_clients.size());
          }
          catch (std::runtime_error& e)
          {
            if (c.socket)
              delete c.socket;
            err(DTR("error accepting new client connection: %s"), e.what());
          }
        }

        void
        handleClients(uint8_t* buf, unsigned int cap)
        {
          // Check for new data from clients.
          ClientList::iterator itr = m_clients.begin();

          while (itr != m_clients.end())
          {
            if (!itr->socket->wasTriggered(m_iom))
            {
              ++itr;
              continue;
            }

            int n;

            try
            {
              n = itr->socket->read((char*)buf, cap);
            }
            catch (std::runtime_error& e)
            {
              closeConnection(*itr, e);
              itr = m_clients.erase(itr);
              continue;
            }

            if (n > 0)
              handleData(itr->parser, buf, n);

            ++itr;
          }
        }
      };
    }
  }
}

DUNE_TASK
