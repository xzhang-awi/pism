// Copyright (C) 2004--2010 Jed Brown, Ed Bueler and Constantine Khroulev
//
// This file is part of PISM.
//
// PISM is free software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later
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

#include <cmath>
#include <cstring>
#include <petscda.h>
#include "iceModel.hh"


//! Allocates SSA tools.
/*
When the SSA becomes an instance of an abstracted class, this may be dealt
within a constructor.
 */
PetscErrorCode IceModel::allocateSSAobjects() {
  PetscErrorCode ierr;

  // mimic IceGrid::createDA() with TRANSPOSE :
  PetscInt dof=2, stencilwidth=1;
  ierr = DACreate2d(grid.com, DA_XYPERIODIC, DA_STENCIL_BOX,
                    grid.My, grid.Mx,
                    grid.Ny, grid.Nx,
                    dof, stencilwidth,
                    grid.procs_y, grid.procs_x,
                    &SSADA); CHKERRQ(ierr);

  ierr = DACreateGlobalVector(SSADA, &SSAX); CHKERRQ(ierr);
  ierr = VecDuplicate(SSAX, &SSARHS); CHKERRQ(ierr);

  ierr = DAGetMatrix(SSADA, MATMPIAIJ, &SSAStiffnessMatrix); CHKERRQ(ierr);

  ierr = KSPCreate(grid.com, &SSAKSP); CHKERRQ(ierr);
  // the default PC type somehow is ILU, which now fails (?) while block jacobi
  //   seems to work; runtime options can override (see test J in vfnow.py)
  PC pc;
  ierr = KSPGetPC(SSAKSP,&pc); CHKERRQ(ierr);
  ierr = PCSetType(pc,PCBJACOBI); CHKERRQ(ierr);
  ierr = KSPSetFromOptions(SSAKSP); CHKERRQ(ierr);

  return 0;
}


//! Deallocate SSA tools.
PetscErrorCode IceModel::destroySSAobjects() {
  PetscErrorCode ierr;

  ierr = KSPDestroy(SSAKSP); CHKERRQ(ierr);
  ierr = MatDestroy(SSAStiffnessMatrix); CHKERRQ(ierr);
  ierr = VecDestroy(SSAX); CHKERRQ(ierr);
  ierr = VecDestroy(SSARHS); CHKERRQ(ierr);
  ierr = DADestroy(SSADA);CHKERRQ(ierr);

  return 0;
}


//! Each step of SSA uses previously saved values to start iteration; zero them here to start.
PetscErrorCode IceModel::initSSA() {
  PetscErrorCode ierr;
  if (!have_ssa_velocities) {
    ierr = vel_ssa.set(0.0); CHKERRQ(ierr);
  }
  return 0;
}


PetscErrorCode IceModel::trivialMoveSSAXtoIMV2V() {
  PetscErrorCode  ierr;
  //  PISMVector2 **Xuv;

  ierr = vel_ssa.copy_from(SSAX); CHKERRQ(ierr); 
  // ierr = vel_ssa.begin_access(); CHKERRQ(ierr);
  // ierr = DAVecGetArray(SSADA,SSAX,&Xuv); CHKERRQ(ierr);
  // for (PetscInt i=grid.xs; i<grid.xs+grid.xm; ++i) {
  //   for (PetscInt j=grid.ys; j<grid.ys+grid.ym; ++j) {
  //     vel_ssa(i,j).u = Xuv[i][j].u;
  //     vel_ssa(i,j).v = Xuv[i][j].v;
  //   }
  // }
  // ierr = DAVecRestoreArray(SSADA,SSAX,&Xuv); CHKERRQ(ierr);
  // ierr = vel_ssa.end_access(); CHKERRQ(ierr);
  return 0;
}


PetscErrorCode IceModel::computeEffectiveViscosity(IceModelVec2S vNuH[2], PetscReal epsilon) {
  PetscErrorCode ierr;

  if (leaveNuHAloneSSA == PETSC_TRUE) {
    return 0;
  }

  bool use_constant_nuh_for_ssa = config.get_flag("use_constant_nuh_for_ssa");
  if (use_constant_nuh_for_ssa) {
    // Intended only for debugging, this treats the entire domain as though
    // it were the strength extension (i.e. strength does not depend on thickness)
    PetscReal nuH = ssaStrengthExtend.notional_strength();
    ierr = vNuH[0].set(nuH); CHKERRQ(ierr);
    ierr = vNuH[1].set(nuH); CHKERRQ(ierr);
    return 0;
  }

  // We need to compute integrated effective viscosity (\bar\nu * H).
  // It is locally determined by the strain rates and temperature field.
  PetscScalar **nuH[2];
  ierr = vH.begin_access(); CHKERRQ(ierr);
  ierr = vNuH[0].get_array(nuH[0]); CHKERRQ(ierr);
  ierr = vNuH[1].get_array(nuH[1]); CHKERRQ(ierr);

  PISMVector2 **uv;
  ierr = vel_ssa.get_array(uv); CHKERRQ(ierr);
  ierr = vWork2dStag.begin_access(); CHKERRQ(ierr);

  const PetscScalar   dx = grid.dx, dy = grid.dy;

  for (PetscInt o=0; o<2; ++o) {
    const PetscInt oi = 1 - o, oj=o;
    for (PetscInt i=grid.xs; i<grid.xs+grid.xm; ++i) {
      for (PetscInt j=grid.ys; j<grid.ys+grid.ym; ++j) {
        const PetscScalar H = 0.5 * (vH(i,j) + vH(i+oi,j+oj));
        if (H < ssaStrengthExtend.min_thickness_for_extension()) {
          // Extends strength of SSA (i.e. nuH coeff) into the ice free region.
          //  Does not add or subtract ice mass.
          nuH[o][i][j] = ssaStrengthExtend.notional_strength();
        } else {
          PetscScalar u_x, u_y, v_x, v_y;
          // Check the offset to determine how to differentiate velocity
          if (o == 0) {
            u_x = (uv[i+1][j].u - uv[i][j].u) / dx;
            u_y = (uv[i][j+1].u + uv[i+1][j+1].u - uv[i][j-1].u - uv[i+1][j-1].u) / (4*dy);
            v_x = (uv[i+1][j].v - uv[i][j].v) / dx;
            v_y = (uv[i][j+1].v + uv[i+1][j+1].v - uv[i][j-1].v - uv[i+1][j-1].v) / (4*dy);
          } else {
            u_x = (uv[i+1][j].u + uv[i+1][j+1].u - uv[i-1][j].u - uv[i-1][j+1].u) / (4*dx);
            u_y = (uv[i][j+1].u - uv[i][j].u) / dy;
            v_x = (uv[i+1][j].v + uv[i+1][j+1].v - uv[i-1][j].v - uv[i-1][j+1].v) / (4*dx);
            v_y = (uv[i][j+1].v - uv[i][j].v) / dy;
          }

          const PetscScalar hardav = vWork2dStag(i,j,o);
          nuH[o][i][j] = H * ice->effectiveViscosity(hardav, u_x, u_y, v_x, v_y);

          if (! finite(nuH[o][i][j]) || false) {
            ierr = PetscPrintf(grid.com, "nuH[%d][%d][%d] = %e\n", o, i, j, nuH[o][i][j]);
              CHKERRQ(ierr); 
            ierr = PetscPrintf(grid.com, "  u_x, u_y, v_x, v_y = %e, %e, %e, %e\n", 
                               u_x, u_y, v_x, v_y);
              CHKERRQ(ierr);
          }
          
          // We ensure that nuH is bounded below by a positive constant.
          nuH[o][i][j] += epsilon;
        } // end of if (vH(i,j) < ssaStrengthExtend.min_thickness_for_extension()) { ... } else {
      } // j
    } // i
  } // o
  ierr = vH.end_access(); CHKERRQ(ierr);
  ierr = vNuH[0].end_access(); CHKERRQ(ierr);
  ierr = vNuH[1].end_access(); CHKERRQ(ierr);

  ierr = vel_ssa.end_access(); CHKERRQ(ierr);
  ierr = vWork2dStag.end_access(); CHKERRQ(ierr);

  // Some communication
  ierr = vNuH[0].beginGhostComm(); CHKERRQ(ierr);
  ierr = vNuH[0].endGhostComm(); CHKERRQ(ierr);
  ierr = vNuH[1].beginGhostComm(); CHKERRQ(ierr);
  ierr = vNuH[1].endGhostComm(); CHKERRQ(ierr);
  return 0;
}


PetscErrorCode IceModel::testConvergenceOfNu(IceModelVec2S vNuH[2], IceModelVec2S vNuHOld[2],
                                             PetscReal *norm, PetscReal *normChange) {
  PetscErrorCode  ierr;
  PetscReal nuNorm[2], nuChange[2];
  const PetscScalar area = grid.dx * grid.dy;
#define MY_NORM     NORM_1

  // Test for change in nu
  ierr = vNuHOld[0].add(-1, vNuH[0]); CHKERRQ(ierr);
  ierr = vNuHOld[1].add(-1, vNuH[1]); CHKERRQ(ierr);

  ierr = vNuHOld[0].norm(MY_NORM, nuChange[0]); CHKERRQ(ierr);
  nuChange[0] *= area;

  ierr = vNuHOld[1].norm(MY_NORM, nuChange[1]); CHKERRQ(ierr);
  nuChange[1] *= area;

  *normChange = sqrt(PetscSqr(nuChange[0]) + PetscSqr(nuChange[1]));

  ierr = vNuH[0].norm(MY_NORM, nuNorm[0]); CHKERRQ(ierr);
  nuNorm[0] *= area;

  ierr = vNuH[1].norm(MY_NORM, nuNorm[1]); CHKERRQ(ierr);
  nuNorm[1] *= area;

  *norm = sqrt(PetscSqr(nuNorm[0]) + PetscSqr(nuNorm[1]));
  return 0;
}


PetscErrorCode IceModel::assembleSSAMatrix(
      bool includeBasalShear, IceModelVec2S vNuH[2], Mat A) {
  PetscErrorCode  ierr;

  const PetscScalar   dx=grid.dx, dy=grid.dy;
  // next constant not too sensitive, but must match value in assembleSSARhs():
  const PetscScalar   scaling = 1.0e9;  // comparable to typical beta for an ice stream
  PetscScalar     **nuH[2], **tauc;
  PISMVector2     **uvssa;

  ierr = MatZeroEntries(A); CHKERRQ(ierr);

  PetscReal beta_shelves_drag_too = config.get("beta_shelves_drag_too");

  /* matrix assembly loop */
  ierr = vMask.begin_access(); CHKERRQ(ierr);
  ierr = vtauc.get_array(tauc); CHKERRQ(ierr);

  ierr = vel_ssa.get_array(uvssa); CHKERRQ(ierr);

  ierr = vNuH[0].get_array(nuH[0]); CHKERRQ(ierr);
  ierr = vNuH[1].get_array(nuH[1]); CHKERRQ(ierr);

  for (PetscInt i=grid.xs; i<grid.xs+grid.xm; ++i) {
    for (PetscInt j=grid.ys; j<grid.ys+grid.ym; ++j) {
      const PismMask mask_value = vMask.value(i,j);
      if (mask_value == MASK_SHEET) {
        // set diagonal entry to one; RHS entry will be known (e.g. SIA) velocity;
        //   this is where boundary value to SSA is set
        MatStencil  row, col;
        row.j = i; row.i = j; row.c = 0;
        col.j = i; col.i = j; col.c = 0;
        ierr = MatSetValuesStencil(A,1,&row,1,&col,&scaling,INSERT_VALUES); CHKERRQ(ierr);
        row.c = 1;
        col.c = 1;
        ierr = MatSetValuesStencil(A,1,&row,1,&col,&scaling,INSERT_VALUES); CHKERRQ(ierr);
      } else {
        const PetscScalar dx2 = dx*dx, d4 = dx*dy*4, dy2 = dy*dy;
        /* Provide shorthand for the following staggered coefficients  nu H:
        *      c11
        *  c00     c01
        *      c10
        * Note that the positive i (x) direction is right and the positive j (y)
        * direction is up. */
        const PetscScalar c00 = nuH[0][i-1][j];
        const PetscScalar c01 = nuH[0][i][j];
        const PetscScalar c10 = nuH[1][i][j-1];
        const PetscScalar c11 = nuH[1][i][j];

        const PetscInt sten = 13;
        MatStencil  row, col[sten];

        /* start with the values at the points */
        PetscScalar valU[] = {
          /*               */ -c11/dy2,
          (2*c00+c11)/d4,     2*(c00-c01)/d4,                 -(2*c01+c11)/d4,
          -4*c00/dx2,         4*(c01+c00)/dx2+(c11+c10)/dy2,  -4*c01/dx2,
          (c11-c10)/d4,                                       (c10-c11)/d4,
          /*               */ -c10/dy2,
          -(2*c00+c10)/d4,    2*(c01-c00)/d4,                 (2*c01+c10)/d4 };
        PetscScalar valV[] = {
          (2*c11+c00)/d4,     (c00-c01)/d4,                   -(2*c11+c01)/d4,
          /*               */ -4*c11/dy2,
          2*(c11-c10)/d4,                                     2*(c10-c11)/d4,
          -c00/dx2,           4*(c11+c10)/dy2+(c01+c00)/dx2,  -c01/dx2,
          -(2*c10+c00)/d4,    (c01-c00)/d4,                   (2*c10+c01)/d4,
          /*               */ -4*c10/dy2 };

        /* Dragging ice experiences friction at the bed determined by the
         *    basalDrag[x|y]() methods.  These may be a plastic, pseudo-plastic,
         *    or linear friction law according to basal->drag(), which gets called
         *    by basalDragx(),basalDragy().  */
        if ((includeBasalShear) && (mask_value == MASK_DRAGGING_SHEET)) {
          // Dragging is done implicitly (i.e. on left side of SSA eqns for u,v).
          valU[5] += basalDragx(tauc, uvssa, i, j);
          valV[7] += basalDragy(tauc, uvssa, i, j);
        }

        // make shelf drag a little bit if desired
        if ((shelvesDragToo == PETSC_TRUE) && (mask_value == MASK_FLOATING)) {
          //ierr = verbPrintf(1,grid.com,"... SHELF IS DRAGGING ..."); CHKERRQ(ierr);
          valU[5] += beta_shelves_drag_too;
          valV[7] += beta_shelves_drag_too;
        }

        // build "u" equation: NOTE TRANSPOSE
        row.j = i; row.i = j; row.c = 0;
        const PetscInt UI[] = {
          /*       */ i,
          i-1,        i,          i+1,
          i-1,        i,          i+1,
          i-1,                    i+1,
          /*       */ i,
          i-1,        i,          i+1};
        const PetscInt UJ[] = {
          /*       */ j+1,
          j+1,        j+1,        j+1,
          j,          j,          j,
          j,                      j,
          /*       */ j-1,
          j-1,        j-1,        j-1};
        const PetscInt UC[] = {
          /*       */ 0,
          1,          1,          1,
          0,          0,          0,
          1,                      1,
          /*       */ 0,
          1,          1,          1};
        for (PetscInt m=0; m<sten; m++) {
          col[m].j = UI[m]; col[m].i = UJ[m], col[m].c = UC[m];
        }
        ierr = MatSetValuesStencil(A,1,&row,sten,col,valU,INSERT_VALUES); CHKERRQ(ierr);

        // build "v" equation: NOTE TRANSPOSE
        row.j = i; row.i = j; row.c = 1;
        const PetscInt VI[] = {
          i-1,        i,          i+1,
          /*       */ i,
          i-1,                    i+1,
          i-1,        i,          i+1,
          i-1,        i,          i+1,
          /*       */ i};
        const PetscInt VJ[] = {
          j+1,        j+1,        j+1,
          /*       */ j+1,
          j,                      j,
          j,          j,          j,
          j-1,        j-1,        j-1,
          /*       */ j-1};
        const PetscInt VC[] = {
          0,          0,          0,
          /*       */ 1,
          0,                      0,
          1,          1,          1,
          0,          0,          0,
          /*       */ 1};
        for (PetscInt m=0; m<sten; m++) {
          col[m].j = VI[m]; col[m].i = VJ[m], col[m].c = VC[m];
        }
        ierr = MatSetValuesStencil(A,1,&row,sten,col,valV,INSERT_VALUES); CHKERRQ(ierr);

      }
    }
  }
  ierr = vMask.end_access(); CHKERRQ(ierr);

  ierr = vel_ssa.end_access(); CHKERRQ(ierr);
  ierr = vtauc.end_access(); CHKERRQ(ierr);

  ierr = vNuH[0].end_access(); CHKERRQ(ierr);
  ierr = vNuH[1].end_access(); CHKERRQ(ierr);

  ierr = MatAssemblyBegin(A, MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
  ierr = MatAssemblyEnd(A, MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
  return 0;
}


PetscErrorCode IceModel::assembleSSARhs(Vec rhs) {
  PetscErrorCode  ierr;

  // next constant not too sensitive, but must match value in assembleSSAMatrix():
  const PetscScalar   scaling = 1.0e9;  // comparable to typical beta for an ice stream;

  ierr = VecSet(rhs, 0.0); CHKERRQ(ierr);

  // get driving stress components
  ierr = computeDrivingStress(vWork2d[0],vWork2d[1]); CHKERRQ(ierr); // in iMgeometry.cc

  PetscScalar     **taudx, **taudy;
  PISMVector2     **rhs_uv;

  ierr = vWork2d[0].get_array(taudx); CHKERRQ(ierr);
  ierr = vWork2d[1].get_array(taudy); CHKERRQ(ierr);
  ierr = vMask.begin_access(); CHKERRQ(ierr);
  ierr = vel_bar.begin_access(); CHKERRQ(ierr);
  ierr = DAVecGetArray(SSADA,rhs,&rhs_uv); CHKERRQ(ierr);
  for (PetscInt i=grid.xs; i<grid.xs+grid.xm; ++i) {
    for (PetscInt j=grid.ys; j<grid.ys+grid.ym; ++j) {
      if (vMask.value(i,j) == MASK_SHEET) {
        rhs_uv[i][j].u = scaling * vel_bar(i,j).u;
        rhs_uv[i][j].v = scaling * vel_bar(i,j).v;
      } else {	// usual case: use already computed driving stress
        rhs_uv[i][j].u = taudx[i][j];
        rhs_uv[i][j].v = taudy[i][j];
      }
    }
  }
  ierr = DAVecRestoreArray(SSADA,rhs,&rhs_uv); CHKERRQ(ierr);
  ierr = vMask.end_access(); CHKERRQ(ierr);
  ierr = vel_bar.end_access(); CHKERRQ(ierr);
  ierr = vWork2d[0].end_access(); CHKERRQ(ierr);
  ierr = vWork2d[1].end_access(); CHKERRQ(ierr);

  ierr = VecAssemblyBegin(rhs); CHKERRQ(ierr);
  ierr = VecAssemblyEnd(rhs); CHKERRQ(ierr);
  return 0;
}


PetscErrorCode IceModel::velocitySSA(PetscInt *numiter) {
  PetscErrorCode ierr;
  IceModelVec2S vNuDefault[2] = {vWork2d[0], vWork2d[1]}; // already allocated space

  ierr = velocitySSA(vNuDefault, numiter); CHKERRQ(ierr);
  return 0;
}


//! Call this one directly if control over allocation of vNuH[2] is needed (e.g. test J).
/*!
Generally use velocitySSA(PetscInt*) unless you have a vNuH[2] already stored away.
 */
PetscErrorCode IceModel::velocitySSA(IceModelVec2S vNuH[2], PetscInt *numiter) {
  PetscErrorCode ierr;
  Mat A = SSAStiffnessMatrix; // solve  A SSAX = SSARHS
  IceModelVec2S vNuHOld[2] = {vWork2d[2], vWork2d[3]};
  PetscReal   norm, normChange;
  PetscInt    its;
  KSPConvergedReason  reason;

  stdout_ssa = "";
  
  PetscReal ssaRelativeTolerance = config.get("ssa_relative_convergence"),
            epsilon              = config.get("epsilon_ssa");

  PetscInt ssaMaxIterations = static_cast<PetscInt>(config.get("max_iterations_ssa"));
  
  ierr = vel_ssa.copy_to(vel_ssa_old); CHKERRQ(ierr);

  // computation of RHS only needs to be done once; does not depend on solution;
  //   but matrix changes under nonlinear iteration (loop over k below)
  ierr = assembleSSARhs(SSARHS); CHKERRQ(ierr);

  ierr = compute_hardav_staggered(vWork2dStag); CHKERRQ(ierr);

  for (PetscInt l=0; ; ++l) { // iterate with increasing regularization parameter
    ierr = computeEffectiveViscosity(vNuH, epsilon); CHKERRQ(ierr);
    ierr = update_nu_viewers(vNuH); CHKERRQ(ierr);
    // iterate on effective viscosity: "outer nonlinear iteration":
    for (PetscInt k=0; k<ssaMaxIterations; ++k) { 
      if (getVerbosityLevel() > 2) {
        char tempstr[50] = "";  snprintf(tempstr,50, "  %d,%2d:", l, k);
        stdout_ssa += tempstr;
      }
    
      // in preparation of measuring change of effective viscosity:
      ierr = vNuH[0].copy_to(vNuHOld[0]); CHKERRQ(ierr);
      ierr = vNuH[1].copy_to(vNuHOld[1]); CHKERRQ(ierr);

      // assemble (or re-assemble) matrix, which depends on updated viscosity
      ierr = assembleSSAMatrix(true, vNuH, A); CHKERRQ(ierr);
      if (getVerbosityLevel() > 2)
        stdout_ssa += "A:";

      // call PETSc to solve linear system by iterative method; "inner linear iteration"
      ierr = KSPSetOperators(SSAKSP, A, A, DIFFERENT_NONZERO_PATTERN); CHKERRQ(ierr);
      ierr = KSPSolve(SSAKSP, SSARHS, SSAX); CHKERRQ(ierr); // SOLVE

      // report to standard out about iteration
      ierr = KSPGetConvergedReason(SSAKSP, &reason); CHKERRQ(ierr);
      if (reason < 0) {
        ierr = verbPrintf(1,grid.com, 
            "\n\n\nPISM ERROR:  KSPSolve() reports 'diverged'; reason = %d = '%s';\n"
                  "  see PETSc man page for KSPGetConvergedReason();   ENDING ...\n\n",
            reason,KSPConvergedReasons[reason]); CHKERRQ(ierr);
        PetscEnd();
      }
      ierr = KSPGetIterationNumber(SSAKSP, &its); CHKERRQ(ierr);
      if (getVerbosityLevel() > 2) {
        char tempstr[50] = "";  snprintf(tempstr,50, "S:%d,%d: ", its, reason);
        stdout_ssa += tempstr;
      }

      // Communicate so that we have stencil width for evaluation of effective
      //   viscosity on next "outer" iteration (and geometry etc. if done):
      ierr = trivialMoveSSAXtoIMV2V(); CHKERRQ(ierr);
      ierr = vel_ssa.beginGhostComm(); CHKERRQ(ierr);
      ierr = vel_ssa.endGhostComm(); CHKERRQ(ierr);
      //OLD:  ierr = moveVelocityToDAVectors(x); CHKERRQ(ierr);

      // update viscosity and check for viscosity convergence
      ierr = computeEffectiveViscosity(vNuH, epsilon); CHKERRQ(ierr);
      ierr = update_nu_viewers(vNuH); CHKERRQ(ierr);
      ierr = testConvergenceOfNu(vNuH, vNuHOld, &norm, &normChange); CHKERRQ(ierr);
      if (getVerbosityLevel() > 2) {
        char tempstr[100] = "";
        snprintf(tempstr,100, "|nu|_2, |Delta nu|_2/|nu|_2 = %10.3e %10.3e\n", 
                         norm, normChange/norm);
        stdout_ssa += tempstr;
      }

      *numiter = k + 1;
      if (norm == 0 || normChange / norm < ssaRelativeTolerance) goto done;

    } // end of the "outer loop" (index: k)

    if (epsilon > 0.0) {
       // this has no units; epsilon goes up by this ratio when previous value failed
       const PetscScalar DEFAULT_EPSILON_MULTIPLIER_SSA = 4.0;
       ierr = verbPrintf(1,grid.com,
			 "WARNING: Effective viscosity not converged after %d iterations\n"
			 "\twith epsilon=%8.2e. Retrying with epsilon * %8.2e.\n",
			 ssaMaxIterations, epsilon, DEFAULT_EPSILON_MULTIPLIER_SSA);
       CHKERRQ(ierr);

       ierr = vel_ssa.copy_from(vel_ssa_old); CHKERRQ(ierr);
       epsilon *= DEFAULT_EPSILON_MULTIPLIER_SSA;
    } else {
       SETERRQ1(1, 
         "Effective viscosity not converged after %d iterations; epsilon=0.0.\n"
         "  Stopping.                \n", 
         ssaMaxIterations);
    }

  } // end of the "outer outer loop" (index: l)

  done:

  if (getVerbosityLevel() > 2) {
    char tempstr[50] = "";
    snprintf(tempstr,50, "... =%5d outer iterations", *numiter);
    stdout_ssa += tempstr;
  } else if (getVerbosityLevel() == 2) {
    // at default verbosity, just record last normchange and iterations
    char tempstr[50] = "";
    snprintf(tempstr,50, "%5d outer iterations", *numiter);
    stdout_ssa += tempstr;
  }
  if (getVerbosityLevel() >= 2)
    stdout_ssa = "  SSA: " + stdout_ssa;
  if (ssaSystemToASCIIMatlab == PETSC_TRUE) {
    ierr = writeSSAsystemMatlab(vNuH); CHKERRQ(ierr);
  }

  return 0;
}

//! \brief Computes f(|v|) as described in [\ref BBssasliding] (page 7, equation 22). 
PetscScalar IceModel::bueler_brown_f(PetscScalar v_squared) {
    const PetscScalar inC_fofv = 1.0e-4 * PetscSqr(secpera),
                    outC_fofv = 2.0 / pi;

    return 1.0 - outC_fofv * atan(inC_fofv * v_squared);
}

//! At all SSA points, update the velocity field.
/*!
Once the vertically-averaged velocity field is computed by the SSA, this 
procedure updates the three-dimensional horizontal velocities \f$u\f$ and
\f$v\f$.  Note that \f$w\f$ gets updated later by 
vertVelocityFromIncompressibility().  The three-dimensional velocity field
is needed, for example, so that the temperature equation can include advection.
Basal velocities also get updated.

Here is where the flag do_superpose controlled by option <tt>-super</tt> applies.
If do_superpose is true then the just-computed velocity \f$v\f$ from the SSA is
combined, in convex combination, to the stored velocity \f$u\f$ from the SIA 
computation:
   \f[U = f(|v|)\, u + \left(1-f(|v|)\right)\, v.\f]
Here
   \f[ f(|v|) = 1 - (2/\pi) \arctan(10^{-4} |v|^2) \f]
is a function which decreases smoothly from 1 for \f$|v| = 0\f$ to 0 as
\f$|v|\f$ becomes significantly larger than 100 m/a.
 */
PetscErrorCode IceModel::broadcastSSAVelocity(bool updateVelocityAtDepth) {

  PetscErrorCode ierr;
  PetscScalar *u, *v;
  
  ierr = vMask.begin_access(); CHKERRQ(ierr);
  ierr = vel_bar.begin_access(); CHKERRQ(ierr);

  PISMVector2 **uvssa, **bvel;
  ierr = vel_ssa.get_array(uvssa); CHKERRQ(ierr);
  ierr = vel_basal.get_array(bvel); CHKERRQ(ierr);

  ierr = u3.begin_access(); CHKERRQ(ierr);
  ierr = v3.begin_access(); CHKERRQ(ierr);

  bool do_superpose = config.get_flag("do_superpose");

  for (PetscInt i=grid.xs; i<grid.xs+grid.xm; ++i) {
    for (PetscInt j=grid.ys; j<grid.ys+grid.ym; ++j) {
      if (vMask.value(i,j) != MASK_SHEET) {
        // combine velocities if desired (and not floating)
        const bool addVels = ( do_superpose && (vMask.value(i,j) == MASK_DRAGGING_SHEET) );
        PetscScalar fv = 0.0, omfv = 1.0;  // case of formulas below where ssa
                                           // speed is infinity; i.e. when !addVels
                                           // we just pass through the SSA velocity
        if (addVels) {
          fv = bueler_brown_f(uvssa[i][j].magnitude_squared());
          omfv = 1 - fv;
        }

        // update 3D velocity; u,v were from SIA
        if (updateVelocityAtDepth) {
          ierr = u3.getInternalColumn(i,j,&u); CHKERRQ(ierr); // returns pointer
          ierr = v3.getInternalColumn(i,j,&v); CHKERRQ(ierr);
          for (PetscInt k=0; k<grid.Mz; ++k) {
            u[k] = (addVels) ? fv * u[k] + omfv * uvssa[i][j].u : uvssa[i][j].u;
            v[k] = (addVels) ? fv * v[k] + omfv * uvssa[i][j].v : uvssa[i][j].v;
          }
        }

        // update basal velocity; ub,vb were from SIA
        bvel[i][j].u = (addVels) ? fv * bvel[i][j].u + omfv * uvssa[i][j].u : uvssa[i][j].u;
        bvel[i][j].v = (addVels) ? fv * bvel[i][j].v + omfv * uvssa[i][j].v : uvssa[i][j].v;
        
        // also update ubar,vbar by adding SIA contribution, interpolated from 
        //   staggered grid
        const PetscScalar ubarSIA = vel_bar(i,j).u,
          vbarSIA = vel_bar(i,j).v;
        vel_bar(i,j).u = (addVels) ? fv * ubarSIA + omfv * uvssa[i][j].u : uvssa[i][j].u;
        vel_bar(i,j).v = (addVels) ? fv * vbarSIA + omfv * uvssa[i][j].v : uvssa[i][j].v;

      }
    }
  }

  ierr = vMask.end_access(); CHKERRQ(ierr);
  ierr = vel_bar.end_access(); CHKERRQ(ierr);
  ierr = vel_ssa.end_access(); CHKERRQ(ierr);
  ierr = vel_basal.end_access(); CHKERRQ(ierr);
  ierr = u3.end_access(); CHKERRQ(ierr);
  ierr = v3.end_access(); CHKERRQ(ierr);

  return 0;
}


//! At SSA points, correct the previously-computed basal frictional heating.
PetscErrorCode IceModel::correctBasalFrictionalHeating() {
  PetscErrorCode  ierr;
  PetscScalar **Rb, **tauc;

  bool use_ssa_velocity = config.get_flag("use_ssa_velocity");

  PISMVector2 **bvel;
  ierr = vel_basal.get_array(bvel); CHKERRQ(ierr);
  ierr = vRb.get_array(Rb); CHKERRQ(ierr);
  ierr = vtauc.get_array(tauc); CHKERRQ(ierr);
  ierr = vMask.begin_access(); CHKERRQ(ierr);

  for (PetscInt i=grid.xs; i<grid.xs+grid.xm; ++i) {
    for (PetscInt j=grid.ys; j<grid.ys+grid.ym; ++j) {
      if (vMask.is_floating(i,j)) {
        Rb[i][j] = 0.0;
      }
      if ((vMask.value(i,j) == MASK_DRAGGING_SHEET) && use_ssa_velocity) {
        // note basalDrag[x|y]() produces a coefficient, not a stress;
        //   uses *updated* ub,vb if do_superpose == TRUE
        const PetscScalar 
	  basal_stress_x = - basalDragx(tauc, bvel, i, j) * bvel[i][j].u,
	  basal_stress_y = - basalDragy(tauc, bvel, i, j) * bvel[i][j].v;
	Rb[i][j] = - basal_stress_x * bvel[i][j].u - basal_stress_y * bvel[i][j].v;
      }
      // otherwise leave SIA-computed value alone
    }
  }

  ierr = vel_basal.end_access(); CHKERRQ(ierr);
  ierr = vtauc.end_access(); CHKERRQ(ierr);
  ierr = vRb.end_access(); CHKERRQ(ierr);
  ierr = vMask.end_access(); CHKERRQ(ierr);

  return 0;
}


//! At SSA points, correct the previously-computed volume strain heating (dissipation heating).
PetscErrorCode IceModel::correctSigma() {
  PetscErrorCode  ierr;
  PetscScalar **H;
  PetscScalar *Sigma, *E;

  double enhancement_factor = config.get("enhancement_factor");

  bool do_superpose = config.get_flag("do_superpose");

  ierr = vH.get_array(H); CHKERRQ(ierr);
  ierr = vMask.begin_access(); CHKERRQ(ierr);
  
  PISMVector2 **uvssa;
  ierr = vel_ssa.get_array(uvssa); CHKERRQ(ierr);

  ierr = Sigma3.begin_access(); CHKERRQ(ierr);
  ierr = Enth3.begin_access(); CHKERRQ(ierr);

  const PetscScalar dx = grid.dx, 
    dy = grid.dy,
    n_glen  = ice->exponent(),
    Sig_pow = (1.0 + n_glen) / (2.0 * n_glen);
  // next constant is the form of regularization used by C. Schoof 2006 "A variational
  // approach to ice streams" J Fluid Mech 556 pp 227--251
  for (PetscInt i=grid.xs; i<grid.xs+grid.xm; ++i) {
    for (PetscInt j=grid.ys; j<grid.ys+grid.ym; ++j) {
      if (vMask.value(i,j) != MASK_SHEET) {
        // note ubar_ssa and vbar_ssa in vel_ssa *are* communicated for differencing by last
        //   call to moveVelocityToDAVectors()
        // apply glaciological-superposition-to-low-order if desired (and not floating)
        bool addVels = ( do_superpose && (vMask.value(i,j) == MASK_DRAGGING_SHEET) );
        PetscScalar fv = 0.0, omfv = 1.0;  // case of formulas below where ssa
                                           // speed is infinity; i.e. when !addVels
                                           // we just pass through the SSA velocity
        if (addVels) {
          fv = bueler_brown_f(uvssa[i][j].magnitude_squared());
          omfv = 1 - fv;
        }
        const PetscScalar 
                u_x   = (uvssa[i+1][j].u - uvssa[i-1][j].u)/(2*dx),
                u_y   = (uvssa[i][j+1].u - uvssa[i][j-1].u)/(2*dy),
                v_x   = (uvssa[i+1][j].v - uvssa[i-1][j].v)/(2*dx),
                v_y   = (uvssa[i][j+1].v - uvssa[i][j-1].v)/(2*dy),
                D2ssa = PetscSqr(u_x) + PetscSqr(v_y) + u_x * v_y
                          + PetscSqr(0.5*(u_y + v_x));
        // get valid pointers to column of Sigma, T values
        ierr = Sigma3.getInternalColumn(i,j,&Sigma); CHKERRQ(ierr);
        ierr = Enth3.getInternalColumn(i,j,&E); CHKERRQ(ierr);
        const PetscInt ks = grid.kBelowHeight(H[i][j]);
        for (PetscInt k=0; k<ks; ++k) {
          // Use hydrostatic pressure; presumably this is not quite right in context
          //   of shelves and streams; here we hard-wire the Glen law
          PetscScalar pressure = EC->getPressureFromDepth(H[i][j]-grid.zlevels[k]),
          // Account for the enhancement factor.
          //   Note, enhancement factor is not used in SSA anyway.
          //   Should we get rid of it completely?  If not, what is most consistent here?
            BofT    = ice->hardnessParameter_from_enth(E[k], pressure) * pow(enhancement_factor,-1/n_glen);
          if (addVels) {

            // extract (D(u)_{13}^2 + D(u)_{23}^2) from Sigma computed earlier:
            const PetscScalar D2sia = pow(Sigma[k] / (2 * BofT), 1.0 / Sig_pow);

            // compute combined D^2 (see section 2.8 of BBssasliding)
            Sigma[k] = 2.0 * BofT * pow(fv*fv*D2sia + omfv*omfv*D2ssa, Sig_pow);

          } else { // floating (or grounded SSA sans super)
            Sigma[k] = 2.0 * BofT * pow(D2ssa, Sig_pow);
          }
        }
        for (PetscInt k=ks+1; k<grid.Mz; ++k) {
          Sigma[k] = 0.0;
        }
      }
      // otherwise leave SIA-computed value alone
    }
  }

  ierr = vH.end_access(); CHKERRQ(ierr);
  ierr = vMask.end_access(); CHKERRQ(ierr);

  ierr = vel_ssa.end_access(); CHKERRQ(ierr);

  ierr = Sigma3.end_access(); CHKERRQ(ierr);
  ierr = Enth3.end_access(); CHKERRQ(ierr);

  return 0;
}

//! Computes vertically-averaged ice hardness on the staggered grid.
PetscErrorCode IceModel::compute_hardav_staggered(IceModelVec2Stag &result) {
  PetscErrorCode ierr;
  PetscScalar *E, *E_ij, *E_offset;

  E = new PetscScalar[grid.Mz];

  ierr = vH.begin_access(); CHKERRQ(ierr);
  ierr = Enth3.begin_access(); CHKERRQ(ierr);
  ierr = result.begin_access(); CHKERRQ(ierr);

  for (PetscInt i=grid.xs; i<grid.xs+grid.xm; ++i) {
    for (PetscInt j=grid.ys; j<grid.ys+grid.ym; ++j) {
      ierr = Enth3.getInternalColumn(i,j,&E_ij); CHKERRQ(ierr);
      for (PetscInt o=0; o<2; o++) {
        const PetscInt oi = 1-o, oj=o;  
        const PetscScalar H = 0.5 * (vH(i,j) + vH(i+oi,j+oj));

        if (H == 0) {
          result(i,j,o) = -1e6; // an obviously impossible value
          continue;
        }

        ierr = Enth3.getInternalColumn(i+oi,j+oj,&E_offset); CHKERRQ(ierr);
        // build a column of enthalpy values a the current location:
        for (int k = 0; k < grid.Mz; ++k) {
          E[k] = 0.5 * (E_ij[k] + E_offset[k]);
        }
        
        result(i,j,o) = ice->averagedHardness_from_enth(H, grid.kBelowHeight(H),
                                                        grid.zlevels, E); CHKERRQ(ierr); 
      } // o
    }   // j
  }     // i

  ierr = result.end_access(); CHKERRQ(ierr);
  ierr = Enth3.end_access(); CHKERRQ(ierr);
  ierr = vH.end_access(); CHKERRQ(ierr);

  delete [] E;

  return 0;
}

