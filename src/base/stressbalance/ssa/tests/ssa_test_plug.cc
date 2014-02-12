// Copyright (C) 2010--2014 Ed Bueler, Constantine Khroulev, and David Maxwell
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

/* This file implements a test case for the ssa: plug flow. The geometry
   consists of a constant surface slope in the positive x-direction, and the
   ice is pinned on the y-boundaries. There is no basal shear stress, and hence
   the the only nonzero terms in the SSA are the "p-laplacian" and the driving
   stress.
*/

static char help[] =
  "\nSSA_TEST_PLUG\n"
  "  Testing program for the finite element implementation of the SSA.\n"
  "  Does a time-independent calculation.  Does not run IceModel or a derived\n"
  "  class thereof.\n\n";

#include "pism_const.hh"
#include "pism_options.hh"
#include "iceModelVec.hh"
#include "flowlaws.hh" // IceFlowLaw
#include "basal_resistance.hh" // IceBasalResistancePlasticLaw
#include "PIO.hh"
#include "NCVariable.hh"
#include "SSAFEM.hh"
#include "SSAFD.hh"
#include "exactTestsIJ.h"
#include "SSATestCase.hh"
#include <math.h>
class SSATestCasePlug: public SSATestCase
{
public:
  SSATestCasePlug(MPI_Comm com, PISMConfig &c, double n)
    : SSATestCase(com, c)
  { 
    H0 = 2000.; //m
    L=50.e3; // 50km half-width
    dhdx = 0.001; // pure number, slope of surface & bed
    tauc0 = 0.; // No basal shear stress
    B0 = 3.7e8; // Pa s^{1/3}; hardness 
               // given on p. 239 of Schoof; why so big?
    this->glen_n = n;      
  }
  
protected:
  virtual PetscErrorCode initializeGrid(int Mx,int My);

  virtual PetscErrorCode initializeSSAModel();

  virtual PetscErrorCode initializeSSACoefficients();

  virtual PetscErrorCode exactSolution(int i, int j, 
    double x, double y, double *u, double *v );


  double H0; // Thickness
  double L;  // Half-width
  double dhdx; // surface slope
  double tauc0; // zero basal shear stress
  double B0;  // hardness
  double glen_n;

  bool dimensionless;
  
};


PetscErrorCode SSATestCasePlug::initializeGrid(int Mx,int My)
{
  double Lx=L, Ly = L; 
  init_shallow_grid(grid,Lx,Ly,Mx,My,NONE);
  return 0;
}


PetscErrorCode SSATestCasePlug::initializeSSAModel()
{
  // Basal sliding law parameters are irrelevant because tauc=0

  // Enthalpy converter is irrelevant (but still required) for this test.
  enthalpyconverter = new EnthalpyConverter(config);

  // Use constant hardness
  config.set_string("ssa_flow_law", "isothermal_glen");
  config.set_double("ice_softness", pow(B0, -glen_n));
  return 0;
}

PetscErrorCode SSATestCasePlug::initializeSSACoefficients()
{
  PetscErrorCode ierr;

  // The finite difference code uses the following flag to treat the non-periodic grid correctly.
  config.set_flag("compute_surf_grad_inward_ssa", true);
  config.set_double("epsilon_ssa", 0.0);

  // Ensure we never use the strength extension.
  ssa->strength_extension->set_min_thickness(H0/2);

  // Set constant coefficients.
  ierr = thickness.set(H0); CHKERRQ(ierr);
  ierr = tauc.set(tauc0); CHKERRQ(ierr);


  // Set boundary conditions (Dirichlet all the way around).
  ierr = bc_mask.set(0.0); CHKERRQ(ierr);
  ierr = vel_bc.begin_access(); CHKERRQ(ierr);
  ierr = bc_mask.begin_access(); CHKERRQ(ierr);
  ierr = bed.begin_access(); CHKERRQ(ierr);
  ierr = surface.begin_access(); CHKERRQ(ierr);
  for (int i=grid.xs; i<grid.xs+grid.xm; ++i) {
    for (int j=grid.ys; j<grid.ys+grid.ym; ++j) {
      double myu, myv;
      const double myx = grid.x[i], myy=grid.y[j];

      bed(i,j) = -myx*(dhdx);
      surface(i,j) = bed(i,j) + H0;
      
      bool edge = ( (j == 0) || (j == grid.My - 1) ) || ( (i==0) || (i==grid.Mx-1) );
      if (edge) {
        bc_mask(i,j) = 1;
        exactSolution(i,j,myx,myy,&myu,&myv);
        vel_bc(i,j).u = myu;
        vel_bc(i,j).v = myv;
      }
    }
  } 
  ierr = vel_bc.end_access(); CHKERRQ(ierr);
  ierr = bc_mask.end_access(); CHKERRQ(ierr);
  ierr = bed.end_access(); CHKERRQ(ierr);
  ierr = surface.end_access(); CHKERRQ(ierr);

  ierr = vel_bc.update_ghosts(); CHKERRQ(ierr);
  ierr = bc_mask.update_ghosts(); CHKERRQ(ierr);
  ierr = bed.update_ghosts(); CHKERRQ(ierr);
  ierr = surface.update_ghosts(); CHKERRQ(ierr);

  ierr = ssa->set_boundary_conditions(bc_mask, vel_bc); CHKERRQ(ierr); 

  return 0;
}

PetscErrorCode SSATestCasePlug::exactSolution(int /*i*/, int /*j*/, 
                                              double /*x*/, double y,
                                              double *u, double *v)
{
  double earth_grav = config.get("standard_gravity"),
    ice_rho = config.get("ice_density");
  double f = ice_rho * earth_grav * H0* dhdx;
  double ynd = y/L;

  *u = 0.5*pow(f,3)*pow(L,4)/pow(B0*H0,3)*(1-pow(ynd,4));
  *v = 0;
  return 0;
}

int main(int argc, char *argv[]) {
  PetscErrorCode  ierr;

  MPI_Comm    com;  // won't be used except for rank,size

  ierr = PetscInitialize(&argc, &argv, PETSC_NULL, help); CHKERRQ(ierr);

  com = PETSC_COMM_WORLD;
  
  /* This explicit scoping forces destructors to be called before PetscFinalize() */
  {  
    PISMUnitSystem unit_system(NULL);
    PISMConfig config(com, "pism_config", unit_system),
      overrides(com, "pism_overrides", unit_system);
    ierr = init_config(com, config, overrides); CHKERRQ(ierr);

    ierr = setVerbosityLevel(5); CHKERRQ(ierr);

    PetscBool usage_set, help_set;
    ierr = PetscOptionsHasName(PETSC_NULL, "-usage", &usage_set); CHKERRQ(ierr);
    ierr = PetscOptionsHasName(PETSC_NULL, "-help", &help_set); CHKERRQ(ierr);
    if ((usage_set==PETSC_TRUE) || (help_set==PETSC_TRUE)) {
      PetscPrintf(com,
                  "\n"
                  "usage of SSA_TEST_PLUG:\n"
                  "  run ssa_test_plug -Mx <number> -My <number> -ssa_method <fd|fem>\n"
                  "\n");
    }

    // Parameters that can be overridden by command line options
    int Mx=11;
    int My=61;
    std::string output_file = "ssa_test_plug.nc";

    std::set<std::string> ssa_choices;
    ssa_choices.insert("fem");
    ssa_choices.insert("fd");
    std::string driver = "fem";

    // double H0dim = 1.;
    double glen_n = 3.;

    ierr = PetscOptionsBegin(com, "", "SSA_TEST_PLUG options", ""); CHKERRQ(ierr);
    {
      bool flag;
      int my_verbosity_level;
      ierr = PISMOptionsInt("-Mx", "Number of grid points in the X direction", 
                                                      Mx, flag); CHKERRQ(ierr);
      ierr = PISMOptionsInt("-My", "Number of grid points in the Y direction", 
                                                      My, flag); CHKERRQ(ierr);
      ierr = PISMOptionsList(com, "-ssa_method", "Algorithm for computing the SSA solution",
                             ssa_choices, driver, driver, flag); CHKERRQ(ierr);
             
      ierr = PISMOptionsString("-o", "Set the output file name", 
                                              output_file, flag); CHKERRQ(ierr);
      ierr = PISMOptionsReal("-ssa_glen_n", "", glen_n, flag ); CHKERRQ(ierr);

      ierr = PISMOptionsInt("-verbose", "Verbosity level",
                            my_verbosity_level, flag); CHKERRQ(ierr);
      if (flag) setVerbosityLevel(my_verbosity_level);
    }
    ierr = PetscOptionsEnd(); CHKERRQ(ierr);

    // Determine the kind of solver to use.
    SSAFactory ssafactory = NULL;
    if(driver == "fem") ssafactory = SSAFEMFactory;
    else if(driver == "fd") ssafactory = SSAFDFactory;
    else { /* can't happen */ }

    SSATestCasePlug testcase(com,config,glen_n);
    ierr = testcase.init(Mx,My,ssafactory); CHKERRQ(ierr);
    ierr = testcase.run(); CHKERRQ(ierr);
    ierr = testcase.report("plug"); CHKERRQ(ierr);
    ierr = testcase.write(output_file); CHKERRQ(ierr);
  }

  ierr = PetscFinalize(); CHKERRQ(ierr);
  return 0;
}
