/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#include <math.h>
#include "min_adaptglok.h"
#include "universe.h"
#include "atom.h"
#include "force.h"
#include "update.h"
#include "output.h"
#include "timer.h"
#include "error.h"

//#include "neighbor.h"
//#include "pair.h"

using namespace LAMMPS_NS;

// EPS_ENERGY = minimum normalization for energy tolerance

#define EPS_ENERGY 1.0e-8

/* Default values defined in min.cpp > Min::Min(LAMMPS *lmp) : Pointers(lmp)
#define DELAYSTEP 20
#define DT_GROW 1.1
#define DT_SHRINK 0.5
#define ALPHA0 0.25
#define ALPHA_SHRINK 0.99
#define TMAX 2.0
#define TMIN 0.02           // as harcoded in IMD: 1/50
*/


/* ---------------------------------------------------------------------- */

MinAdaptGlok::MinAdaptGlok(LAMMPS *lmp) : Min(lmp) {}

/* ---------------------------------------------------------------------- */

void MinAdaptGlok::init()
{
  Min::init();

  dt = update->dt;
  dtinit = dt;
  dtmax = TMAX * dt;
  dtmin = TMIN * dt;
  alpha = ALPHA0;
  ntimestep_fire = last_negative = update->ntimestep;
}

/* ---------------------------------------------------------------------- */

void MinAdaptGlok::setup_style()
{
  double **v = atom->v;
  int nlocal = atom->nlocal;

  for (int i = 0; i < nlocal; i++)
    v[i][0] = v[i][1] = v[i][2] = 0.0;
}

/* ----------------------------------------------------------------------
   set current vector lengths and pointers
   called after atoms have migrated
------------------------------------------------------------------------- */

void MinAdaptGlok::reset_vectors()
{
  // atomic dof

  nvec = 3 * atom->nlocal;
  if (nvec) xvec = atom->x[0];
  if (nvec) fvec = atom->f[0];
}

/* ---------------------------------------------------------------------- */

int MinAdaptGlok::iterate(int maxiter)
{
  bigint ntimestep;
  double vmax,vdotf,vdotfall,vdotv,vdotvall,fdotf,fdotfall;
  double scale1,scale2;
  double dtvone,dtv,dtf,dtfm;
  int flag,flagall;

  double *rmass = atom->rmass;
  double *mass = atom->mass;
  int *type = atom->type;

  double **f = atom->f;
  double **v = atom->v;

  alpha_final = 0.0;

  for (int iter = 0; iter < maxiter; iter++) {

    if (timer->check_timeout(niter))
      return TIMEOUT;

    ntimestep = ++update->ntimestep;
    niter++;

    // vdotfall = v dot f

    int nlocal = atom->nlocal;

    vdotf = 0.0;
    for (int i = 0; i < nlocal; i++)
      vdotf += v[i][0]*f[i][0] + v[i][1]*f[i][1] + v[i][2]*f[i][2];
    MPI_Allreduce(&vdotf,&vdotfall,1,MPI_DOUBLE,MPI_SUM,world);

    // sum vdotf over replicas, if necessary
    // this communicator would be invalid for multiprocess replicas

    if (update->multireplica == 1) {
      vdotf = vdotfall;
      MPI_Allreduce(&vdotf,&vdotfall,1,MPI_DOUBLE,MPI_SUM,universe->uworld);
    }

    // if (v dot f) > 0:
    // v = (1-alpha) v + alpha |v| Fhat
    // |v| = length of v, Fhat = unit f
    // The modificatin of v is made wihtin the Verlet integration, after v update
    // if more than DELAYSTEP since v dot f was negative:
    // increase timestep and decrease alpha

    if (vdotfall > 0.0) {
      vdotv = 0.0;
      for (int i = 0; i < nlocal; i++)
        vdotv += v[i][0]*v[i][0] + v[i][1]*v[i][1] + v[i][2]*v[i][2];
      MPI_Allreduce(&vdotv,&vdotvall,1,MPI_DOUBLE,MPI_SUM,world);

      // sum vdotv over replicas, if necessary
      // this communicator would be invalid for multiprocess replicas

      if (update->multireplica == 1) {
        vdotv = vdotvall;
        MPI_Allreduce(&vdotv,&vdotvall,1,MPI_DOUBLE,MPI_SUM,universe->uworld);
      }

      fdotf = 0.0;
      for (int i = 0; i < nlocal; i++)
        fdotf += f[i][0]*f[i][0] + f[i][1]*f[i][1] + f[i][2]*f[i][2];
      MPI_Allreduce(&fdotf,&fdotfall,1,MPI_DOUBLE,MPI_SUM,world);

      // sum fdotf over replicas, if necessary
      // this communicator would be invalid for multiprocess replicas

      if (update->multireplica == 1) {
        fdotf = fdotfall;
        MPI_Allreduce(&fdotf,&fdotfall,1,MPI_DOUBLE,MPI_SUM,universe->uworld);
      }

      scale1 = 1.0 - alpha;
      if (fdotfall == 0.0) scale2 = alpha;
      else scale2 = alpha * sqrt(vdotvall/fdotfall);

      if (ntimestep - last_negative > DELAYSTEP) {
        dt = MIN(dt*DT_GROW,dtmax);
        update->dt = dt;
        alpha *= ALPHA_SHRINK;
      }

    // else (v dot f) <= 0
    // if more than DELAYSTEP since starting the relaxation:
    // reset alpha
    //    if dt > dtmin:
    //    decrease timestep
    // set x(t) = x(t-0.5*dt)
    // set v = 0

    } else {
      last_negative = ntimestep;
      // Limit decrease of timestep
      if (ntimestep - ntimestep_fire > DELAYSTEP) {
        alpha = ALPHA0;
        if (dt > dtmin) {
          dt *= DT_SHRINK;
          update->dt = dt;
        }
      }
      double **x = atom->x;
      for (int i = 0; i < nlocal; i++) {
        x[i][0] -= 0.5 * dtv * v[i][0];
        x[i][1] -= 0.5 * dtv * v[i][1];
        x[i][2] -= 0.5 * dtv * v[i][2];
      }
      for (int i = 0; i < nlocal; i++)
        v[i][0] = v[i][1] = v[i][2] = 0.0;
    }

    // limit timestep so no particle moves further than dmax

    dtvone = dt;

    for (int i = 0; i < nlocal; i++) {
      vmax = MAX(fabs(v[i][0]),fabs(v[i][1]));
      vmax = MAX(vmax,fabs(v[i][2]));
      if (dtvone*vmax > dmax) dtvone = dmax/vmax;
    }
    MPI_Allreduce(&dtvone,&dtv,1,MPI_DOUBLE,MPI_MIN,world);

    // min dtv over replicas, if necessary
    // this communicator would be invalid for multiprocess replicas

    if (update->multireplica == 1) {
      dtvone = dtv;
      MPI_Allreduce(&dtvone,&dtv,1,MPI_DOUBLE,MPI_MIN,universe->uworld);
    }
    
  // Velocity Verlet integration

  double **x = atom->x;
  dtf = 0.5 * dtv * force->ftm2v;

  if (rmass) {
    for (int i = 0; i < nlocal; i++) {
        dtfm = dtf / rmass[i];
        v[i][0] += dtfm * f[i][0];
        v[i][1] += dtfm * f[i][1];
        v[i][2] += dtfm * f[i][2];
        if (vdotfall > 0.0) {
          v[i][0] = scale1*v[i][0] + scale2*f[i][0];
          v[i][1] = scale1*v[i][1] + scale2*f[i][1];
          v[i][2] = scale1*v[i][2] + scale2*f[i][2];
        }
        x[i][0] += dtv * v[i][0];
        x[i][1] += dtv * v[i][1];
        x[i][2] += dtv * v[i][2];
    }
  } else {
    for (int i = 0; i < nlocal; i++) {
        dtfm = dtf / mass[type[i]];
        v[i][0] += dtfm * f[i][0];
        v[i][1] += dtfm * f[i][1];
        v[i][2] += dtfm * f[i][2];
        if (vdotfall > 0.0) {
          v[i][0] = scale1*v[i][0] + scale2*f[i][0];
          v[i][1] = scale1*v[i][1] + scale2*f[i][1];
          v[i][2] = scale1*v[i][2] + scale2*f[i][2];
        }
        x[i][0] += dtv * v[i][0];
        x[i][1] += dtv * v[i][1];
        x[i][2] += dtv * v[i][2];
    }
  }
  
  double **f = atom->f;
  double **v = atom->v;

  if (rmass) {
    for (int i = 0; i < nlocal; i++) {
        dtfm = dtf / rmass[i];
        v[i][0] += dtfm * f[i][0];
        v[i][1] += dtfm * f[i][1];
        v[i][2] += dtfm * f[i][2];
      }
  } else {
    for (int i = 0; i < nlocal; i++) {
      dtfm = dtf / mass[type[i]];
      v[i][0] += dtfm * f[i][0];
      v[i][1] += dtfm * f[i][1];
      v[i][2] += dtfm * f[i][2];
    }
  }

    eprevious = ecurrent;
    ecurrent = energy_force(0);
    neval++;

    // energy tolerance criterion
    // only check after DELAYSTEP elapsed since velocties reset to 0
    // sync across replicas if running multi-replica minimization

    if (update->etol > 0.0 && ntimestep-last_negative > DELAYSTEP) {
      if (update->multireplica == 0) {
        if (fabs(ecurrent-eprevious) <
            update->etol * 0.5*(fabs(ecurrent) + fabs(eprevious) + EPS_ENERGY)) {
          update->dt = dtinit;
          return ETOL;
        }
      } else {
        if (fabs(ecurrent-eprevious) <
            update->etol * 0.5*(fabs(ecurrent) + fabs(eprevious) + EPS_ENERGY))
          flag = 0;
        else flag = 1;
        MPI_Allreduce(&flag,&flagall,1,MPI_INT,MPI_SUM,universe->uworld);
        if (flagall == 0) {
          update->dt = dtinit;
          return ETOL;
        }
      }
    }

    // force tolerance criterion
    // sync across replicas if running multi-replica minimization

    if (update->ftol > 0.0) {
      fdotf = fnorm_sqr();
      if (update->multireplica == 0) {
        if (fdotf < update->ftol*update->ftol) {
          update->dt = dtinit;
          return FTOL;
        }
      } else {
        if (fdotf < update->ftol*update->ftol) flag = 0;
        else flag = 1;
        MPI_Allreduce(&flag,&flagall,1,MPI_INT,MPI_SUM,universe->uworld);
        if (flagall == 0) {
          update->dt = dtinit;
          return FTOL;
        }
      }
    }

    // output for thermo, dump, restart files

    if (output->next == ntimestep) {
      timer->stamp();
      output->write(ntimestep);
      timer->stamp(Timer::OUTPUT);
    }
  }

  update->dt = dtinit;
  return MAXITER;
}
