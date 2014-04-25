// Copyright (C) 2008-2014 Ed Bueler, Constantine Khroulev, Ricarda Winkelmann,
// Gudfinna Adalgeirsdottir and Andy Aschwanden
//
// This file is part of PISM.
//
// PISM is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 3 of the License, or (at your option) any later
// version.
//
// PISM is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License
// along with PISM; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

// Implementation of the atmosphere model using constant-in-time precipitation
// and a cosine yearly cycle for near-surface air temperatures.

// This includes the SeaRISE Greenland parameterization.

#include "PASeariseGreenland.hh"
#include "PISMVars.hh"
#include "IceGrid.hh"
#include "pism_options.hh"
#include "PISMTime.hh"
#include <assert.h>
#include "PISMConfig.hh"

namespace pism {

///// PA_SeaRISE_Greenland

PA_SeaRISE_Greenland::PA_SeaRISE_Greenland(IceGrid &g, const Config &conf)
  : PAYearlyCycle(g, conf) {
  // empty
}

PA_SeaRISE_Greenland::~PA_SeaRISE_Greenland() {
}

PetscErrorCode PA_SeaRISE_Greenland::init(Vars &vars) {
  PetscErrorCode ierr;

  m_t = m_dt = GSL_NAN;  // every re-init restarts the clock

  ierr = verbPrintf(2, grid.com,
                    "* Initializing SeaRISE-Greenland atmosphere model based on the Fausto et al (2009)\n"
                    "  air temperature parameterization and using stored time-independent precipitation...\n");
  CHKERRQ(ierr);

  m_reference =
    "R. S. Fausto, A. P. Ahlstrom, D. V. As, C. E. Boggild, and S. J. Johnsen, 2009. "
    "A new present-day temperature parameterization for Greenland. J. Glaciol. 55 (189), 95-105.";

  bool precip_file_set = false;
  ierr = PetscOptionsBegin(grid.com, "",
                           "-atmosphere searise_greenland options", ""); CHKERRQ(ierr);
  {
    std::string option_prefix = "-atmosphere_searise_greenland";
    ierr = OptionsString(option_prefix + "_file",
                             "Specifies a file with boundary conditions",
                             m_precip_filename, precip_file_set); CHKERRQ(ierr);
  }
  ierr = PetscOptionsEnd(); CHKERRQ(ierr);

  if (precip_file_set == true) {
    m_variables = &vars;

    ierr = verbPrintf(2, grid.com,
                      "  * Option '-atmosphere_searise_greenland %s' is set...\n",
                      m_precip_filename.c_str());
    CHKERRQ(ierr);

    ierr = PAYearlyCycle::init_internal(m_precip_filename,
                                        true, /* do regrid */
                                        0 /* start (irrelevant) */); CHKERRQ(ierr);
  } else {
    ierr = PAYearlyCycle::init(vars); CHKERRQ(ierr);
  }

  // initialize pointers to fields the parameterization depends on:
  m_surfelev = dynamic_cast<IceModelVec2S*>(vars.get("surface_altitude"));
  if (!m_surfelev) SETERRQ(grid.com, 1, "ERROR: surface_altitude is not available");

  m_lat = dynamic_cast<IceModelVec2S*>(vars.get("latitude"));
  if (!m_lat) SETERRQ(grid.com, 1, "ERROR: latitude is not available");

  m_lon = dynamic_cast<IceModelVec2S*>(vars.get("longitude"));
  if (!m_lon) SETERRQ(grid.com, 1, "ERROR: longitude is not available");

  return 0;
}

PetscErrorCode PA_SeaRISE_Greenland::precip_time_series(int i, int j, double *values) {

  for (unsigned int k = 0; k < m_ts_times.size(); k++)
    values[k] = m_precipitation(i,j);

  return 0;
}

//! \brief Updates mean annual and mean July near-surface air temperatures.
//! Note that the precipitation rate is time-independent and does not need
//! to be updated.
PetscErrorCode PA_SeaRISE_Greenland::update(double my_t, double my_dt) {
  PetscErrorCode ierr;

  if (m_lat->metadata().has_attribute("missing_at_bootstrap")) {
    ierr = PetscPrintf(grid.com, "PISM ERROR: latitude variable was missing at bootstrap;\n"
                       "  SeaRISE-Greenland atmosphere model depends on latitude and would return nonsense!!\n");
    CHKERRQ(ierr);
    PISMEnd();
  }
  if (m_lon->metadata().has_attribute("missing_at_bootstrap")) {
    ierr = PetscPrintf(grid.com, "PISM ERROR: longitude variable was missing at bootstrap;\n"
                       "  SeaRISE-Greenland atmosphere model depends on longitude and would return nonsense!!\n");
    CHKERRQ(ierr);
    PISMEnd();
  }

  if ((fabs(my_t - m_t) < 1e-12) &&
      (fabs(my_dt - m_dt) < 1e-12))
    return 0;

  m_t  = my_t;
  m_dt = my_dt;

  const double 
    d_ma     = config.get("snow_temp_fausto_d_ma"),      // K
    gamma_ma = config.get("snow_temp_fausto_gamma_ma"),  // K m-1
    c_ma     = config.get("snow_temp_fausto_c_ma"),      // K (degN)-1
    kappa_ma = config.get("snow_temp_fausto_kappa_ma"),  // K (degW)-1
    d_mj     = config.get("snow_temp_fausto_d_mj"),      // SAME UNITS as for _ma ...
    gamma_mj = config.get("snow_temp_fausto_gamma_mj"),
    c_mj     = config.get("snow_temp_fausto_c_mj"),
    kappa_mj = config.get("snow_temp_fausto_kappa_mj");

  IceModelVec2S &h = *m_surfelev, &lat_degN = *m_lat, &lon_degE = *m_lon;

  ierr = h.begin_access();   CHKERRQ(ierr);
  ierr = lat_degN.begin_access(); CHKERRQ(ierr);
  ierr = lon_degE.begin_access(); CHKERRQ(ierr);
  ierr = m_air_temp_mean_annual.begin_access();  CHKERRQ(ierr);
  ierr = m_air_temp_mean_july.begin_access();  CHKERRQ(ierr);

  for (int i = grid.xs; i<grid.xs+grid.xm; ++i) {
    for (int j = grid.ys; j<grid.ys+grid.ym; ++j) {
      m_air_temp_mean_annual(i,j) = d_ma + gamma_ma * h(i,j) + c_ma * lat_degN(i,j) + kappa_ma * (-lon_degE(i,j));
      m_air_temp_mean_july(i,j)   = d_mj + gamma_mj * h(i,j) + c_mj * lat_degN(i,j) + kappa_mj * (-lon_degE(i,j));
    }
  }

  ierr = h.end_access();   CHKERRQ(ierr);
  ierr = lat_degN.end_access(); CHKERRQ(ierr);
  ierr = lon_degE.end_access(); CHKERRQ(ierr);
  ierr = m_air_temp_mean_annual.end_access();  CHKERRQ(ierr);
  ierr = m_air_temp_mean_july.end_access();  CHKERRQ(ierr);

  return 0;
}

} // end of namespace pism
