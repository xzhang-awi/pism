// Copyright (C) 2009--2015 Constantine Khroulev
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

#include "iceModelVec.hh"
#include "pism_const.hh"
#include "IceGrid.hh"

#include "error_handling.hh"
#include "iceModelVec_helpers.hh"
#include "PISMConfigInterface.hh"

#include "pism_memory.hh"
using PISM_SHARED_PTR_NSPACE::dynamic_pointer_cast;

namespace pism {

IceModelVec2V::IceModelVec2V() : IceModelVec2() {
  m_dof = 2;
  begin_end_access_use_dof = false;
}

IceModelVec2V::Ptr IceModelVec2V::ToVector(IceModelVec::Ptr input) {
  IceModelVec2V::Ptr result = dynamic_pointer_cast<IceModelVec2V,IceModelVec>(input);
  if (not (bool)result) {
    throw RuntimeError("dynamic cast failure");
  }
  return result;
}

void IceModelVec2V::create(const IceGrid &my_grid, const std::string &short_name,
                           IceModelVecKind ghostedp,
                           unsigned int stencil_width) {

  IceModelVec2::create(my_grid, short_name, ghostedp,
                       stencil_width, m_dof);

  UnitSystem sys = m_grid->config.unit_system();

  m_metadata[0] = SpatialVariableMetadata(sys, "u" + short_name, *m_grid);
  m_metadata[1] = SpatialVariableMetadata(sys, "v" + short_name, *m_grid);

  m_name = "vel" + short_name;
}

Vector2** IceModelVec2V::get_array() {
  begin_access();
  return static_cast<Vector2**>(array);
}

void IceModelVec2V::add(double alpha, const IceModelVec &x) {
  return add_2d<IceModelVec2V>(this, alpha, &x, this);
}

void IceModelVec2V::add(double alpha, const IceModelVec &x, IceModelVec &result) const {
  return add_2d<IceModelVec2V>(this, alpha, &x, &result);
}

void IceModelVec2V::copy_from(const IceModelVec &source) {
  return copy_2d<IceModelVec2V>(&source, this);
}

} // end of namespace pism
