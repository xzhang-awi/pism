/* Copyright (C) 2016, 2017 PISM Authors
 *
 * This file is part of PISM.
 *
 * PISM is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * PISM is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PISM; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef PSINITIALIZATION_H
#define PSINITIALIZATION_H

#include "Modifier.hh"

namespace pism {
namespace surface {

/*! Surface model "modifier" that helps with initialization.
 *
 * This modifier saves *all* fields a surface model provides as a part of the model state and
 * re-loads them during initialization so that they are available *before* the first time step in a
 * re-started run.
 *
 * It is
 *
 * - not visible to the user,
 * - is added automatically, and
 * - does not have a corresponding "keyword" in surface::Factory.
 */
class InitializationHelper : public SurfaceModifier {
public:
  InitializationHelper(IceGrid::ConstPtr g, SurfaceModel* in);
protected:
  void init_impl();
  void update_impl(double my_t, double my_dt);
  void attach_atmosphere_model_impl(atmosphere::AtmosphereModel *in);

  void mass_flux_impl(IceModelVec2S &result) const;
  void temperature_impl(IceModelVec2S &result) const;
  void liquid_water_fraction_impl(IceModelVec2S &result) const;
  void layer_mass_impl(IceModelVec2S &result) const;
  void layer_thickness_impl(IceModelVec2S &result) const;

  void define_model_state_impl(const PIO &output) const;
  void write_model_state_impl(const PIO &output) const;

private:
  // store pointers to fields so that we can iterate over them
  std::vector<IceModelVec*> m_variables;
  // storage
  IceModelVec2S m_ice_surface_mass_flux;
  IceModelVec2S m_ice_surface_temperature;
  IceModelVec2S m_ice_surface_liquid_water_fraction;
  IceModelVec2S m_surface_layer_mass;
  IceModelVec2S m_surface_layer_thickness;
};

} // end of namespace surface
} // end of namespace pism


#endif /* PSINITIALIZATION_H */
