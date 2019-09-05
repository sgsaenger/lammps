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

/* ----------------------------------------------------------------------
   Contributing author: Sebastian Gsänger (FAU)
------------------------------------------------------------------------- */

#include "fix_pw.h"

#include "atom.h"
#include "comm.h"
#include "error.h"
#include "force.h"
#include "group.h"
#include "memory.h"
#include "modify.h"
#include "universe.h"
#include "update.h"

#include <algorithm>

using namespace LAMMPS_NS;
using namespace FixConst;

extern "C" {
void start_pw(MPI_Fint comm, int nimage, int npool, int ntaskgroup, int nband, int ndiag,
              const char *input_file, const char *output_file,
              int* nat);
void update_pw(double *x);
void calc_pw(double *f);
void end_pw(int *exit_status);
double energy_pw(void);
}

/* ---------------------------------------------------------------------- */

FixPW::FixPW(LAMMPS *l, int narg, char **arg):
    Fix{l, narg, arg}
{
  if (narg < 4) error->all(FLERR, "Illegal fix qe/pw command");

  if (atom->tag_enable == 0)
    error->all(FLERR,"fix qe/pw requires atom IDs");

  // ensure that only one fix is active per partition
  if (modify->find_fix_by_style("qe/pw") != -1)
    error->all(FLERR, "Only one instance of fix qe/pw allowed at a time");

  // make sure we understand eachother
  // NOTE: other formats also need additional scaling for positions
  if (strcmp(update->unit_style, "metal") == 0){
    fscale = 1.0/23.0609;
  }else if (strcmp(update->unit_style, "real") == 0){
    fscale = 1.0;
  }else error->all(FLERR, "fix qe/pw requires real or metal units");

  // save file-name for later
  inp_file = arg[3];

  // parse additional arguments
  int iarg = 4;
  while(iarg < narg-1){
    if(strcmp(arg[iarg], "npool") == 0){
      npool = force->inumeric(FLERR, arg[iarg+1]);
      iarg += 2;
    }else if(strcmp(arg[iarg], "ntask") == 0){
      ntask = force->inumeric(FLERR, arg[iarg+1]);
      iarg += 2;
    }else if(strcmp(arg[iarg], "nband") == 0){
      nband = force->inumeric(FLERR, arg[iarg+1]);
      iarg += 2;
    }else if(strcmp(arg[iarg], "ndiag") == 0){
      ndiag = force->inumeric(FLERR, arg[iarg+1]);
      iarg += 2;
    }else if(strcmp(arg[iarg], "ec") == 0){
      ec_group = group->find(arg[iarg+1]);
      if(comm->me == 0)
        error->warning(FLERR, "Electrostatic coupling is not efficient");
      if(ec_group < 0){
        error->all(FLERR, "Invalid group given as argument to 'ec'");
      }
      if(ec_group == igroup){
        error->all(FLERR, "EC group must differ from main group of fix qe/pw");
      }
      iarg += 2;
    }else if(strcmp(arg[iarg], "link") == 0){
      int nlink = force->inumeric(FLERR, arg[iarg+1]);
      linkatoms.resize(nlink);
      for(int i=0; i<nlink; i+=2){
        linkatoms[i] = {force->inumeric(FLERR, arg[iarg+2+i]),
                        -1,
                        force->numeric(FLERR, arg[iarg+3+i])};
      }
      auto last = std::unique(linkatoms.begin(), linkatoms.end(),
                              [](const auto& l, const auto& r)
                              {return l.link_atom == r.link_atom;});
      if(last != linkatoms.end()){
        error->all(FLERR, "Duplicate entries in list of link atoms");
      }
      iarg += 2+2*nlink;
    }else if(strcmp(arg[iarg], "scheme") == 0){
      if(strcmp(arg[iarg+1], "Z1") == 0){
        charge_scheme = charge_t::Z1;
      }else if(strcmp(arg[iarg+1], "Z2") == 0){
        charge_scheme = charge_t::Z2;
      }else if(strcmp(arg[iarg+1], "Z3") == 0){
        charge_scheme = charge_t::Z3;
      }else if(strcmp(arg[iarg+1], "RCD") == 0){
        charge_scheme = charge_t::RCD;
      }else if(strcmp(arg[iarg+1], "CS") == 0){
        charge_scheme = charge_t::CS;
      }else{
        error->all(FLERR, "Invalid charge redistribution scheme");
      }
      iarg += 2;
    }else{
      error->all(FLERR, "Illegal fix qe/pw command");
    }
  }

  // collect atoms bound to ID on proc 0
  auto getBoundNeighbors = [this](tagint tag, int mask, int dist=0){
    int idx = atom->map(tag);
    std::vector<tagint> list;
    if(idx != -1){
      // collect bound neighbors in requested group
      const auto* const neighbors = atom->special[idx];
      for(int i=0; i<atom->nspecial[idx][dist]; ++i){
        if(atom->map(neighbors[i]) == -1){
          error->one(FLERR, "Bound neighbor not a local or ghost atom");
        }
        if(atom->mask[atom->map(neighbors[i])] & mask){
          list.push_back(neighbors[i]);
        }
      }
      // if tagged atom is on another process, transfer it to proc 0
      if(comm->me != 0){
        auto s = static_cast<int>(list.size());
        MPI_Send(&s, 1, MPI_INT, 0, tag, world);
        MPI_Send(list.data(), s, MPI_LMP_TAGINT, 0, tag, world);
      }
    }else if((comm->me == 0) && (idx == -1)){
      // if tagged atom is on another process, transfer it to proc 0
      int s{};
      MPI_Recv(&s, 1, MPI_INT, MPI_ANY_SOURCE, tag, world, MPI_STATUS_IGNORE);
      list.resize(s);
      MPI_Recv(list.data(), s, MPI_LMP_TAGINT, MPI_ANY_SOURCE, tag, world, MPI_STATUS_IGNORE);
    }
    return list;
  };
  // check link and modified-charge atoms
  if (!linkatoms.empty()) {
    if(!(atom->molecular)){
      error->all(FLERR, "Link atoms can only be used when bonds are declared");
    }
    // make sure map from global to local ids is initialized
    int mapflag{0};
    if(!atom->map_style){
      mapflag = 1;
      atom->map_init();
      atom->map_set();
    }
    const auto qmmask = group->bitmask[igroup];
    for(auto& link: linkatoms){
      // check if link atom is in QM group
      auto idx = atom->map(link.link_atom);
      if((idx != -1) && !(atom->mask[idx] & qmmask)){
          char msg[50];
          sprintf(msg, "Link atom %d is not included in QM group.", link.link_atom);
          error->one(FLERR, msg);
      }
      auto neighbors = getBoundNeighbors(link.link_atom, qmmask);
      if(comm->me == 0){
        if(neighbors.empty()){
          char msg[50];
          sprintf(msg, "Link atom %d has no neighbors in QM group.", link.link_atom);
          error->one(FLERR, msg);
        }else if(neighbors.size()>1){
          char msg[50];
          sprintf(msg, "Link atom %d is bound to multiple QM atoms.", link.link_atom);
          error->one(FLERR, msg);
        }else{
          link.qm_atom = neighbors.front();
        }
      }
    }
    // if needed, find atoms whose charges need to be modified
    if(ec_group != -1){
      const auto ecmask = group->bitmask[ec_group];
      if(charge_scheme == charge_t::Z1){
        // only ignore charge of link-atoms,
        // which is assumed to be excluded from EC group
      }else if(charge_scheme == charge_t::Z2){
        // ignore charge of 1-2 neighbors
        for(const auto& link: linkatoms){
          auto neighbors = getBoundNeighbors(link.link_atom, ecmask);
          for(const auto& n: neighbors){
            chargeatoms.push_back({n, -1});
          }
        }
      }else if(charge_scheme == charge_t::Z3){
        // ignore charge of 1-2 and 1-3 neighbors
        for(const auto& link: linkatoms){
          auto neighbors = getBoundNeighbors(link.link_atom, ecmask, 1);
          for(const auto& n: neighbors){
            chargeatoms.push_back({n, -1});
          }
        }
      }
      if((charge_scheme == charge_t::RCD) || (charge_scheme == charge_t::CS)){
        // create virtual point charges depending on 1-2 neighbors
        for(const auto& link: linkatoms){
          auto neighbors = getBoundNeighbors(link.link_atom, ecmask);
          for(const auto& n: neighbors){
            chargeatoms.push_back({n, link.link_atom});
          }
        }
      }
    }
    // de-init map if we initialized it
    if(mapflag){
      atom->map_delete();
      atom->map_style = 0;
    }
  }

  // redirect stdout when PWScf is running
  out_file = "log.pwscf";
  if(universe->existflag){
    out_file += '.' + std::to_string(universe->iworld);
  }

  // calculate number of EC atoms
  if(ec_group != -1){
    switch(charge_scheme){
    case charge_t::Z1:
      nec = group->count(ec_group);
      break;
    case charge_t::Z2:
    case charge_t::Z3:
      nec = group->count(ec_group) - chargeatoms.size();
      break;
    case charge_t::RCD:
      nec = group->count(ec_group) + chargeatoms.size();
      break;
    case charge_t::CS:
      nec = group->count(ec_group) + 2*chargeatoms.size();
      break;
    default:
      error->all(FLERR, "Invalid charge redistribution scheme");
    }
  }

  // initialize PWScf, receive number of qm-atoms
  int nat{0};
  nqm = group->count(igroup);
  if(nqm > MAXSMALLINT){
    error->all(FLERR, "Too many QM atoms for fix qe/pw.");
  }

  start_pw(MPI_Comm_c2f(world), 1, npool, ntask, nband, ndiag,
           inp_file.data(), out_file.data(), &nat);

  // check if pw has been launched succesfully and with compatible input
  /* NOTE: using one, not sure if a stray pwscf process may run off.
   * may be if pw had been compiled against a different MPI version.
   * if so, may be resolved if building pw is done via lammps' buildsystem.
   */
  if (nat<0){
    error->one(FLERR, "Error opening output file for fix qe/pw.");
  }else if( nat != nqm){
    error->one(FLERR, "Mismatching number of atoms in fix qe/pw.");
  }

  // collect and save tags of main group
  recv_count_buf = new int[universe->nprocs];
  displs_buf = new int[universe->nprocs];

  // collect process-local tags
  decltype (tags) tmp{};
  tmp.reserve(static_cast<size_t>(nqm));
  for(int i=0; i<atom->nlocal; ++i){
    if(atom->mask[i] & groupbit)
      tmp.push_back(atom->tag[i]);
  }

  // gather number of tags
  int send_count = tmp.size() * sizeof(decltype (tmp)::value_type);
  MPI_Allgather(&send_count, 1, MPI_INT, recv_count_buf, 1, MPI_INT, world);

  // construct displacement
  displs_buf[0] = 0;
  for(int i=1; i<universe->nprocs; ++i){
    displs_buf[i] = displs_buf[i-1]+recv_count_buf[i-1];
  }

  // gather and sort tags
  tags.resize(static_cast<size_t>(nqm));
  MPI_Allgatherv(tmp.data(), send_count, MPI_BYTE,
                 tags.data(), recv_count_buf, displs_buf, MPI_BYTE, world);
  std::sort(tags.begin(), tags.end());

  // assign tags to qm-id
  for(int i=0; i<nqm; ++i){
    hash[tags[i]] = i;
  }
}

FixPW::~FixPW()
{
  int result;
  end_pw(&result);

  delete [] recv_count_buf;
  delete [] displs_buf;
  memory->destroy(double_buf);
}

int FixPW::setmask()
{
  return POST_FORCE
       | MIN_POST_FORCE
       | POST_INTEGRATE
       | MIN_PRE_FORCE
       | THERMO_ENERGY
  ;
}

double FixPW::compute_scalar()
{
  return energy_pw();
}

// TODO: collective communications may be optimized (away?)

void FixPW::min_post_force(int i)
{
  post_force(i);
}

void FixPW::post_force(int)
{
  // run scf and receive forces
  calc_pw(double_buf[0]);
  // redistribute forces of link atoms
  for(const auto& l: linkatoms){
    double* forceL = double_buf[hash[l.link_atom]];
    double* forceQ = double_buf[hash[l.qm_atom]];
    forceQ[0] += (1-l.ratio)*forceL[0];
    forceQ[1] += (1-l.ratio)*forceL[1];
    forceQ[2] += (1-l.ratio)*forceL[2];
    forceL[0] *= l.ratio;
    forceL[1] *= l.ratio;
    forceL[2] *= l.ratio;
  }
  // communicate forces to other lammps processes
  if(comm->me == 0){
    for(int i=0; i<nqm; ++i){
      comm_buf[i] = {i, double_buf[i][0],
                     double_buf[i][1], double_buf[i][2]};
    }
  }
  MPI_Bcast(comm_buf.data(), nqm*sizeof(decltype(comm_buf)::value_type), MPI_BYTE, 0, world);
  // add forces
  const tagint * const tag = atom->tag;
  double ** f = atom->f;
  for(int i=0; i<atom->nlocal; ++i){
    for(auto& dat: comm_buf){
      if(tag[i] == dat.tag){
        f[i][0] += dat.x*fscale;
        f[i][1] += dat.y*fscale;
        f[i][2] += dat.z*fscale;
      }
    }
  }
}

void FixPW::min_pre_force(int)
{
  post_integrate();
}

void FixPW::post_integrate()
{
  // collect coordinates
  const int *const mask = atom->mask;
  const tagint * const tag = atom->tag;
  const double * const * const x = atom->x;

  // collect atom positions in buffer
  decltype(comm_buf) tmp;
  tmp.reserve(static_cast<size_t>(nqm));
  for(int i=0; i<atom->nlocal; ++i){
    if(mask[i] & groupbit){
      tmp.push_back({hash.at(tag[i]), x[i][0], x[i][1], x[i][2]});
    }
  }
  // gather number of local atoms
  int send_count = tmp.size() * sizeof(decltype (tmp)::value_type);
  MPI_Allgather(&send_count, 1, MPI_INT, recv_count_buf, 1, MPI_INT, world);
  // construct displacement
  displs_buf[0] = 0;
  for(int i=1; i<universe->nprocs; ++i){
    displs_buf[i] = displs_buf[i-1]+recv_count_buf[i-1];
  }
  // gather comm_buf
  comm_buf.resize(static_cast<size_t>(nqm));
  MPI_Allgatherv(tmp.data(), send_count, MPI_BYTE,
                 comm_buf.data(), recv_count_buf, displs_buf, MPI_BYTE, world);

  // extract data to contiguous memory
  memory->grow(double_buf, static_cast<int>(nqm), 3, "pw/qe:double_buf");
  for(auto& dat: comm_buf){
    double_buf[dat.tag][0] = dat.x;
    double_buf[dat.tag][1] = dat.y;
    double_buf[dat.tag][2] = dat.z;
  }
  // modify positions of link atoms
  for(const auto& l: linkatoms){
    double* posL = double_buf[hash[l.link_atom]];
    double* posQ = double_buf[hash[l.qm_atom]];
    posL[0] = posQ[0] + l.ratio * (posL[0]-posQ[0]);
    posL[1] = posQ[1] + l.ratio * (posL[1]-posQ[1]);
    posL[2] = posQ[2] + l.ratio * (posL[2]-posQ[2]);
  }

  MPI_Barrier(world);
  // transmit to PWScf
  update_pw(double_buf[0]);
}
