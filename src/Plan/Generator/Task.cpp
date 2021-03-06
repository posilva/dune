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
// Author: José Pinto                                                       *
//***************************************************************************

// ISO C++ 98 headers.
#include <stdexcept>

// DUNE headers.
#include <DUNE/DUNE.hpp>

using DUNE_NAMESPACES;

namespace Plan
{
  //! This task accepts and processes messages of type PlanGeneration,
  //! resulting in the generation of corresponding plans.
  //! The idea is simple: According to the plan identifier and an
  //! optional set of parameters, this task generates a
  //! PlanSpecification that is sent to the plan database and
  //! (optionally) is also executed immediately (through a PlanControl
  //! message).
  //!
  //! @author José Pinto
  namespace Generator
  {
    struct Arguments
    {
      //! The depth of the generated Loiter maneuvers.
      double dive_depth;
      //! The depth of the generated Goto maneuvers.
      double travel_depth;
      //! The radius to be used in Loiter, StationKeeping and Elevator maneuvers.
      double radius;
      //! Ammount of time, in seconds to stay underwater for the 'dive' generator.
      int dive_time;
      //! The speed (in RPMS) to be commanded when generating maneuvers.
      int speed_rpms;
    };

    struct Task: public DUNE::Tasks::Task
    {
      //! Task arguments.
      Arguments m_args;
      //! Stores the last estimated state;
      IMC::EstimatedState* m_estate;
      //! Stores the last LblConfig message
      IMC::LblConfig* m_last_lbl_config;
      //! map for storing last received announces
      std::map<unsigned int, IMC::Announce> m_last_announces;

      //! Class constructor
      Task(const std::string& name, Tasks::Context& ctx):
        DUNE::Tasks::Task(name, ctx),
        m_estate(NULL),
        m_last_lbl_config(NULL)
      {
        param("Dive depth", m_args.dive_depth)
        .description("Depth to dive in response to 'dive' command")
        .defaultValue("2.0");

        param("Traveling depth", m_args.travel_depth)
        .description("Depth to use when traveling (Goto maneuvers)")
        .defaultValue("1.0");

        param("Dive seconds", m_args.dive_time)
        .description("Time to be under water in response to 'dive' command (seconds)")
        .defaultValue("300");

        param("Loiter radius", m_args.radius)
        .description("Radius of generated loiter and station keeping maneuvers")
        .defaultValue("10.0");

        param("RPM Speed", m_args.speed_rpms)
        .description("Speed in RPMs to be used in the generated maneuvers")
        .defaultValue("1000");

        bind<IMC::Announce>(this);
        bind<IMC::PlanGeneration>(this);
        bind<IMC::EstimatedState>(this);
        bind<IMC::LblConfig>(this);
      }

      //! Frees memory associated with stored messages.
      void
      onResourceRelease(void)
      {
        Memory::clear(m_estate);
        Memory::clear(m_last_lbl_config);
      }

      //! Stores the announce in the map (one announce per system id).
      void
      consume(const IMC::Announce* msg)
      {
        m_last_announces[msg->getSource()] = *msg;
      }

      //! Stores the last received EstimatedState message.
      void
      consume(const IMC::EstimatedState* msg)
      {
        if (m_estate == NULL)
        {
          setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_ACTIVE);
        }
        else
        {
          delete m_estate;
          m_estate = NULL;
        }

        m_estate = new IMC::EstimatedState(*msg);
      }

      //! Stores the last received LblConfig message.
      void
      consume(const IMC::LblConfig* msg)
      {
        if (msg->op != IMC::LblConfig::OP_SET_CFG)
          return;

        if (m_last_lbl_config)
        {
          delete m_last_lbl_config;
          m_last_lbl_config = NULL;
        }
        m_last_lbl_config = new IMC::LblConfig(*msg);
      }

      //! Handles a PlanGeneration message. According to the 'plan_id' may result
      //! in the generation of different plans:
      //! <em>go_home</em>:
      //!   - Goto maneuver 50 meters from EstimatedState reference location, followed by a StationKeeping
      //!     on top of the current location (derived from EstimatedState).
      //!
      //! <em>surface</em>:
      //!   - Elevator at the current position, followed by a StationKeeping at the surface
      //!
      //! <em>dive</em>:
      //!   - Loiter at the current position (radius, time, depth and speed are configuration parameters)
      //!
      //! <em>go</em>:
      //!   - This template looks in the 'params' tuplelist for the arguments loc, lat and lon. If loc is
      //!     set, then it will look for a beacon with same name (in which cases uses its location), a
      //!     an announced system with the same name (in which case uses its location) or the string "home"
      //!     (in which case uses the EstimatedState reference location). If lat / lon arguments are set, uses those as the
      //!     coordinates of the location where to go.
      //!     The resulting plan is a Goto maneuver to the target location, followed by a station keeping in
      //!     the same location.
      //!
      //! <em>sk</em>:
      //!   - This template is similar to 'go' but generates only the maneuver StationKeeping at the desired
      //!     location.
      void
      consume(const IMC::PlanGeneration* msg)
      {
        if (msg->op != IMC::PlanGeneration::OP_REQUEST)
          return;

        TupleList tlist(msg->params, "=", ";", true);

        // try to generate corresponding plan
        IMC::PlanSpecification spec;

        // if generation is not possible, send an error back.
        if (!generate(msg->plan_id, tlist, spec))
        {
          IMC::PlanGeneration response;
          response.cmd = msg->cmd;
          response.op = msg->IMC::PlanGeneration::OP_ERROR;
          response.plan_id = msg->plan_id;
          response.params = "error=Given plan id was not understood.";

          inf(DTR("Unable to generate plan using template %s"), msg->plan_id.c_str());
          dispatch(response);

          return;
        }

        if (msg->cmd == IMC::PlanGeneration::CMD_GENERATE || msg->cmd == IMC::PlanGeneration::CMD_EXECUTE)
        {
          IMC::PlanDB pdb;
          pdb.op = IMC::PlanDB::DBOP_SET;
          pdb.type = IMC::PlanDB::DBT_REQUEST;
          pdb.plan_id = spec.plan_id;
          pdb.arg.set(spec);
          pdb.request_id = 0;

          dispatch(pdb);
        }

        if (msg->cmd == IMC::PlanGeneration::CMD_EXECUTE)
        {
          IMC::PlanControl pcontrol;
          pcontrol.arg.set(spec);
          pcontrol.info = DTR("Plan generated automatically");

          if (tlist.get("calibrate") != "false")
            pcontrol.flags = IMC::PlanControl::FLG_CALIBRATE;

          pcontrol.plan_id = spec.plan_id;
          pcontrol.request_id = 0;
          pcontrol.type = IMC::PlanControl::PC_REQUEST;
          pcontrol.op = IMC::PlanControl::PC_START;
          dispatch(pcontrol);
        }

        IMC::PlanGeneration response;
        response.cmd = msg->cmd;
        response.op = msg->IMC::PlanGeneration::OP_SUCCESS;
        response.plan_id = msg->plan_id;
        response.params = msg->params;
        dispatch(response);
      }

      void
      onMain(void)
      {
        while (!stopping())
        {
          waitForMessages(1.0);
        }
      }

      //! This (utilitary) method calculates the current position using the
      //! last received state.
      //! @param[out] lat where the current latitude will be stored
      //! @param[out] lon where the current longitude will be stored
      //! @param[out] depth where the current depth will be stored
      //! @returns true if it was possible to get position or false otherwise.
      bool
      getCurrentPosition(double* lat, double* lon, double* depth)
      {
        if (m_estate == NULL)
          return false;

        *lat = m_estate->lat;
        *lon = m_estate->lon;
        *depth = m_estate->depth;

        Coordinates::toWGS84(*m_estate, *lat, *lon);

        return true;
      }

      //! This (utility) method generates a PlanSpecification
      //! consisting in the given maneuver sequence.
      //! @param[in] plan_id The name of the plan to be generated
      //! @param[in] maneuvers A vector with maneuvers (order of the resulting plan will correspond
      //! to the order of this vector).
      //! @param[out] result The resulting PlanSpecification will be stored here.
      void
      sequentialPlan(std::string plan_id, const IMC::MessageList<IMC::Maneuver>* maneuvers, IMC::PlanSpecification& result)
      {
        IMC::PlanManeuver last_man;

        IMC::MessageList<IMC::Maneuver>::const_iterator itr;
        unsigned i = 0;
        for (itr = maneuvers->begin(); itr != maneuvers->end(); itr++, i++)
        {
          if (*itr == NULL)
            continue;

          IMC::PlanManeuver man_spec;

          man_spec.data.set(*(*itr));
          man_spec.maneuver_id = String::str(i + 1);
          if (itr == maneuvers->begin())
          {
            // no transitions.
          }
          else
          {
            IMC::PlanTransition trans;
            trans.conditions = "maneuverIsDone";
            trans.dest_man = man_spec.maneuver_id;
            trans.source_man = last_man.maneuver_id;

            result.transitions.push_back(trans);
          }

          result.maneuvers.push_back(man_spec);

          last_man = man_spec;
        }

        result.plan_id = plan_id;
        result.description = DTR("Plan generated automatically by DUNE.");
        result.start_man_id = "1";
      }

      //! This (utility) method updates a position if the name of the
      //! beacon found in the given message matches the string we are
      //! looking for (match parameter).
      //! @param[in] match the string we are looking for.
      //! @param[in] msg inline message with beacon position.
      //! @param[out] lat where the latitude will be updated
      //! @param[out] lon where the longitude will be updated
      void
      updatePositionFromBeacon(const std::string& match, const IMC::LblBeacon* msg, double* lat, double* lon)
      {
        if (msg == NULL)
          return;

        if (msg != NULL && msg->beacon == match)
        {
          *lat = msg->lat;
          *lon = msg->lon;
        }
      }

      //! This method parses a string and a list of parameters and
      //! eventually generates a corresponding plan.
      //! @param[in] plan_id the string to be parsed (command).
      //! @param[in] params a tuple list with parameters to be used in
      //! the generation.
      //! @param[out] result where to store the resulting plan
      //! specification.
      //! @returns true if a plan was actually generated or false if
      //! result wasn't touched.
      bool
      generate(const std::string& plan_id, TupleList& params, IMC::PlanSpecification& result)
      {
        result.plan_id = plan_id;
        result.description = DTR("Plan generated automatically by DUNE.");

        inf(DTR("generating plan from '%s' template..."), plan_id.c_str());

        // This template generates a plan that goes to a location and stays there (station keeping)
        if (plan_id == "go" || plan_id == "sk")
        {
          // The location can be given either by the 'loc' parameter or 'lat' / 'lon'
          std::string loc = params.get("loc");
          double lat = Angles::radians(params.get("lat", 0.0));
          double lon = Angles::radians(params.get("lon", 0.0));

          // Searches for a beacon whose name matches given loc
          // in which case updates lat/lon with the beacon's location
          if (loc != "" && m_last_lbl_config != NULL)
          {
            IMC::MessageList<IMC::LblBeacon>::const_iterator itr = m_last_lbl_config->beacons.begin();
            for (unsigned i = 0; itr != m_last_lbl_config->beacons.end(); ++itr, ++i)
              updatePositionFromBeacon(loc, *itr, &lat, &lon);
          }

          // Searches for a system with a name that matches given loc
          // in which case updates lat/lon with the system's location
          if (loc != "" && !m_last_announces.empty())
          {
            std::map<unsigned int, IMC::Announce>::iterator i;
            for (i = m_last_announces.begin(); i != m_last_announces.end(); i++)
            {
              IMC::Announce an = i->second;
              if (loc == an.sys_name)
              {
                lat = an.lat;
                lon = an.lon;
                break;
              }
            }
          }

          // if 'loc' is home, updates lat/lon with home location
          if (loc == "home" && m_estate != NULL)
          {
            lat = m_estate->lat;
            lon = m_estate->lon;
          }

          // if some latitude / longitude was given, goes there
          if (lat != 0 || lon != 0)
          {
            IMC::MessageList<IMC::Maneuver> maneuvers;

            // only generates a goto if the template is go, otherwise generates only a station keeping
            if (plan_id == "go")
            {
              IMC::Goto* go_near = new IMC::Goto();
              go_near->lat = lat;
              go_near->lon = lon;
              go_near->z = m_args.travel_depth;
              go_near->z_units = IMC::Z_DEPTH;
              go_near->speed_units = IMC::SUNITS_RPM;
              go_near->speed = m_args.speed_rpms;
              maneuvers.push_back(*go_near);

              delete go_near;
            }

            IMC::StationKeeping* at_surface = new IMC::StationKeeping();
            at_surface->duration = 0;
            at_surface->lat = lat;
            at_surface->lon = lon;
            at_surface->speed_units = IMC::SUNITS_RPM;
            at_surface->speed = m_args.speed_rpms;
            at_surface->radius = m_args.radius;
            maneuvers.push_back(*at_surface);

            delete at_surface;

            sequentialPlan(plan_id, &maneuvers, result);

            return true;
          }
        }

        // This template makes the vehicle go to the home referential
        // (partly using goto and last 50 meters using a station keeping).
        if (plan_id == "go_home")
        {
          if (m_estate == NULL)
            return false;

          double bearing, range, home_lat, home_lon, cur_lat, cur_lon, cur_depth, near_lat, near_lon;

          getCurrentPosition(&cur_lat, &cur_lon, &cur_depth);
          near_lat = home_lat = m_estate->lat;
          near_lon = home_lon = m_estate->lon;

          WGS84::getNEBearingAndRange(cur_lat, cur_lon, home_lat, home_lon, &bearing, &range);

          if (range > 50)
          {
            double n, e, ratio;
            WGS84::displacement(home_lat, home_lon, 0.0, cur_lat, cur_lon, 0.0, &n, &e);
            ratio = 50 / range;
            WGS84::displace(n * ratio, e * ratio, &near_lat, &near_lon);
          }

          IMC::MessageList<IMC::Maneuver> maneuvers;

          IMC::Goto* go_near = new IMC::Goto();
          go_near->lat = near_lat;
          go_near->lon = near_lon;
          go_near->z = m_args.travel_depth;
          go_near->z_units = IMC::Z_DEPTH;
          go_near->speed_units = IMC::SUNITS_RPM;
          go_near->speed = m_args.speed_rpms;
          maneuvers.push_back(*go_near);

          delete go_near;

          IMC::StationKeeping* at_surface = new IMC::StationKeeping();
          at_surface->duration = 0;
          at_surface->lat = home_lat;
          at_surface->lon = home_lon;
          at_surface->z = 0.0;
          at_surface->z_units = IMC::Z_DEPTH;
          at_surface->speed_units = IMC::SUNITS_RPM;
          at_surface->speed = m_args.speed_rpms;
          at_surface->radius = m_args.radius;
          maneuvers.push_back(*at_surface);

          delete at_surface;

          sequentialPlan(plan_id, &maneuvers, result);

          return true;
        }

        // This template makes the vehicle dive for some time
        if (plan_id == "dive")
        {
          double lat, lon, depth;
          getCurrentPosition(&lat, &lon, &depth);

          IMC::MessageList<IMC::Maneuver> maneuvers;

          IMC::Loiter* loiter = new IMC::Loiter();
          loiter->lat = lat;
          loiter->lon = lon;
          loiter->z = m_args.dive_depth;
          loiter->z_units = IMC::Z_DEPTH;
          loiter->type = IMC::Loiter::LT_CIRCULAR;
          loiter->duration = m_args.dive_time;
          loiter->speed = m_args.speed_rpms;
          loiter->speed_units = IMC::SUNITS_RPM;
          loiter->radius = m_args.radius;
          maneuvers.push_back(*loiter);

          delete loiter;

          sequentialPlan(plan_id, &maneuvers, result);

          return true;
        }

        // This template makes the vehicle come to the surface and wait
        // above its current position
        if (plan_id == "surface")
        {
          IMC::MessageList<IMC::Maneuver> maneuvers;

          double lat, lon, depth;
          getCurrentPosition(&lat, &lon, &depth);

          IMC::Elevator* surface = new IMC::Elevator();
          surface->flags = IMC::Elevator::FLG_CURR_POS;
          surface->end_z = 0;
          surface->end_z_units = IMC::Z_DEPTH;
          surface->lat = lat;
          surface->lon = lon;
          surface->radius = m_args.radius;
          surface->speed = m_args.speed_rpms;
          surface->speed_units = IMC::SUNITS_RPM;
          maneuvers.push_back(*surface);

          delete surface;

          IMC::StationKeeping* at_surface = new IMC::StationKeeping();
          at_surface->duration = 0;
          at_surface->lat = lat;
          at_surface->lon = lon;
          at_surface->z = 0.0;
          at_surface->z_units = IMC::Z_DEPTH;
          at_surface->speed_units = IMC::SUNITS_RPM;
          at_surface->speed = m_args.speed_rpms;
          at_surface->radius = m_args.radius;
          maneuvers.push_back(*at_surface);

          delete at_surface;

          sequentialPlan(plan_id, &maneuvers, result);

          return true;
        }

        // in the case the template is not understood, returns false
        return false;
      }
    };
  }
}

DUNE_TASK
