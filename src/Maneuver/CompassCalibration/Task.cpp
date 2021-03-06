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
// Author: Pedro Calado                                                     *
//***************************************************************************

// DUNE headers.
#include <DUNE/DUNE.hpp>

namespace Maneuver
{
  namespace CompassCalibration
  {
    using DUNE_NAMESPACES;

    //! Minimum radius admissible for loitering
    static const float c_min_radius = 5.0f;

    struct Arguments
    {
      //! Saturation level for variation in pitch references.
      double variation;
      //! AHRS entity label.
      std::string label_ahrs;
      //! Tolerance in cross-track error to consider loiter has started
      float cross_tol;
      //! Number of 360 degree turns until calibration
      float turns;
    };

    struct Task: public DUNE::Maneuvers::Maneuver
    {
      //! Desired Path message.
      IMC::DesiredPath m_path;
      //! Desired Pitch for the maneuver.
      IMC::DesiredPitch m_pitch;
      //! EstimatedState.
      IMC::EstimatedState m_estate;
      //! Magnetic Field for Compass Calibration.
      IMC::MagneticField m_mfield;
      //! Compass calibration algorithm.
      DUNE::Navigation::CompassCalibration m_ccal;
      //! Yoyo motion controller.
      DUNE::Control::YoYoMotion* m_yoyo;
      //! Z units for the maneuver.
      IMC::ZUnits m_zunits;
      //! End time of the maneuver.
      double m_end_time;
      //! Duration of the maneuver
      uint16_t m_duration;
      //! In calibrating phase
      bool m_calibrating;
      //! AHRS entity id.
      unsigned m_ahrs_eid;
      //! Started yoyo movements (not necessarily calibrating)
      bool m_yoyo_ing;
      //! Last value of psi
      float m_last_psi;
      //! Accumulated psi variation
      float m_accum_psi;
      //! Task arguments.
      Arguments m_args;

      Task(const std::string& name, Tasks::Context& ctx):
        DUNE::Maneuvers::Maneuver(name, ctx),
        m_yoyo(NULL),
        m_calibrating(false),
        m_yoyo_ing(false)
      {
        param("Maximum Pitch Variation", m_args.variation)
        .defaultValue("10.0")
        .units(Units::Degree)
        .description("Maximum pitch variation step when changing z direction");

        param("Entity Label - Compass", m_args.label_ahrs)
        .defaultValue("AHRS")
        .description("Entity label of 'AHRS' messages");

        param("Cross Track Tolerance", m_args.cross_tol)
        .defaultValue("1.0")
        .units(Units::Meter)
        .description("Tolerance in cross-track error to consider loiter has started");

        param("Number Of Turns", m_args.turns)
        .defaultValue("1.0")
        .description("Number of 360 degree turns until calibration");

        bindToManeuver<Task, IMC::CompassCalibration>();
        bind<IMC::PathControlState>(this);
        bind<IMC::EstimatedState>(this);
        bind<IMC::EulerAngles>(this);
        bind<IMC::MagneticField>(this);
      }

      void
      onUpdateParameters(void)
      {
        m_args.variation = Angles::radians(m_args.variation);
      }

      void
      onResourceRelease(void)
      {
        Memory::clear(m_yoyo);
      }

      void
      onEntityResolution(void)
      {
        m_ahrs_eid = resolveEntity(m_args.label_ahrs);
        m_mfield.setDestinationEntity(m_ahrs_eid);
      }

      void
      consume(const IMC::CompassCalibration* maneuver)
      {
        setControl(IMC::CL_PATH);

        if (maneuver->radius < c_min_radius)
        {
          war(DTR("invalid loiter radius, forcing a minimum of %0.2f"), c_min_radius);
          m_path.lradius = c_min_radius;
        }
        else
        {
          m_path.lradius = maneuver->radius;
        }

        m_path.end_lat = maneuver->lat;
        m_path.end_lon = maneuver->lon;
        m_path.end_z = maneuver->z;
        m_path.end_z_units = maneuver->z_units;

        m_zunits = static_cast<IMC::ZUnits>(maneuver->z_units);

        if (maneuver->direction == IMC::CompassCalibration::LD_CCLOCKW)
          m_path.flags = IMC::DesiredPath::FL_CCLOCKW;
        else
          m_path.flags = 0;  // clockwise by default

        m_path.speed = maneuver->speed;
        m_path.speed_units = maneuver->speed_units;

        m_duration = maneuver->duration;

        // set but do not send
        m_pitch.value = 0;

        dispatch(m_path);

        m_end_time = -1;
        m_calibrating = false;
        m_yoyo_ing = false;

        double zref;

        if (m_zunits == IMC::Z_DEPTH)
        {
          zref = maneuver->z;
        }
        else if (m_zunits == IMC::Z_ALTITUDE)
        {
          zref = -maneuver->z;
        }
        else
        {
          signalError("unsupported vertical reference");
          return;
        }

        // initialize yoyo motion controller
        Memory::clear(m_yoyo);

        m_yoyo = new YoYoMotion(this, maneuver->pitch, zref,
                                maneuver->amplitude, m_args.variation);
      }

      void
      consume(const IMC::EstimatedState* msg)
      {
        m_estate = *msg;

        if (!m_yoyo_ing)
          return;

        yoyoMotion(false);
      }

      void
      consume(const IMC::EulerAngles* msg)
      {
        // Update Direct Cosine Matrix.
        if (msg->getSourceEntity() == m_ahrs_eid)
          m_ccal.updateDCM(*msg);
      }

      void
      consume(const IMC::MagneticField* msg)
      {
        // Update stabilized magnetic field.
        if (msg->getSourceEntity() == m_ahrs_eid)
          m_ccal.updateField(*msg);
      }

      void
      consume(const IMC::PathControlState* pcs)
      {
        if ((pcs->flags & IMC::PathControlState::FL_LOITERING) && !m_yoyo_ing)
        {
          setControl(IMC::CL_PATH | IMC::CL_PITCH);

          m_path.flags |= IMC::DesiredPath::FL_NO_Z;

          yoyoMotion(true);
          m_yoyo_ing = true;

          dispatch(m_path);
        }
        else if ((pcs->flags & IMC::PathControlState::FL_LOITERING) && m_yoyo_ing && !m_calibrating)
        {
          bool inbounds = false;

          if (m_path.end_z_units == IMC::Z_ALTITUDE)
            inbounds = m_yoyo->isBetweenBounds(-m_estate.alt);
          else if (m_path.end_z_units == IMC::Z_DEPTH)
            inbounds = m_yoyo->isBetweenBounds(m_estate.depth);

          if (inbounds)
          {
            m_calibrating = true;
            m_last_psi = m_estate.psi;
            m_accum_psi = 0.0;
          }
        }
        else if ((pcs->flags & IMC::PathControlState::FL_LOITERING) && m_calibrating)
        {
          if (m_duration)
          {
            double now = Clock::get();

            if (m_end_time < 0)
            {
              m_end_time = now + m_duration;
              debug("will now loiter for %d seconds", m_duration);
            }
            else if (now >= m_end_time)
            {
              calibrate();

              signalCompletion();
              return;
            }
            else
            {
              signalProgress((uint16_t)Math::round(m_end_time - now));
            }
          }

          m_accum_psi += Angles::normalizeRadian(m_estate.psi - m_last_psi);
          m_last_psi = m_estate.psi;

          if (((m_path.flags & IMC::DesiredPath::FL_CCLOCKW) && (m_accum_psi < - m_args.turns * c_two_pi)) ||
              (!(m_path.flags & IMC::DesiredPath::FL_CCLOCKW) && (m_accum_psi > m_args.turns * c_two_pi)))
          {
            debug("described %.1f turns for calibration", m_args.turns);

            calibrate();

            signalCompletion();
            return;
          }
        }
        else
        {
          if (m_duration > 0)
            signalProgress(pcs->eta + m_duration);
          else
            signalProgress();
        }
      }

      //! Run compass calibration.
      void
      calibrate(void)
      {
        Math::Matrix params = m_ccal.getCalibrationParams();

        // Fill message and send to bus.
        m_mfield.x = params(0);
        m_mfield.y = params(1);
        m_mfield.z = params(2);
        dispatch(m_mfield);
      }

      //! Yoyo motion update
      //! @param[in] startup whether or not the motion is starting right now
      void
      yoyoMotion(bool startup)
      {
        double state_z;

        if (m_zunits == IMC::Z_DEPTH)
        {
          state_z = m_estate.depth;
        }
        else if ((m_estate.alt >= 0) && (m_zunits == IMC::Z_ALTITUDE))
        {
          state_z = - m_estate.alt;
        }
        else
        {
          signalNoAltitude();
          return;
        }

        double v = m_yoyo->update(startup, state_z, m_estate.theta);

        if (v == m_pitch.value)
          return;

        // Dispatch pitch message
        m_pitch.value = v;
        dispatch(m_pitch);
      }
    };
  }
}

DUNE_TASK
