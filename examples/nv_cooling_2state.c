#include <math.h>
#include <stdlib.h>
#include "quac.h"
#include "operators.h"
#include "solver.h"
#include "petsc.h"
#include "dm_utilities.h"

PetscErrorCode ts_monitor(TS,PetscInt,PetscReal,Vec,void*);
FILE *f_pop;

int main(int argc,char **args){
  PetscInt N_th,num_phonon,num_nv,init_phonon,steady_state_solve,steps_max,full_H_space;
  PetscReal time_max,dt;
  double w_m,D_e,Omega,gamma_eff,lambda_eff,lambda_s,gamma_par;
  double Q,alpha,kHz,MHz,GHz,THz,Hz,rate;
  operator a,nv;
  Vec rho;


  /* Initialize QuaC */
  QuaC_initialize(argc,args);


  /* Define units */
  GHz = 1e3;
  MHz = GHz*1e-3;
  kHz = MHz*1e-3;
  THz = GHz*1e3;
  Hz  = kHz*1e-3;
  alpha      = 0.01663;
  N_th       = 2;
  num_phonon = 5;
  num_nv     = 2;
  init_phonon = 4;
  steady_state_solve = 1;
  /* Get arguments from command line */
  PetscOptionsGetInt(NULL,NULL,"-num_nv",&num_nv,NULL);
  PetscOptionsGetInt(NULL,NULL,"-num_phonon",&num_phonon,NULL);
  PetscOptionsGetInt(NULL,NULL,"-n_th",&N_th,NULL);
  PetscOptionsGetInt(NULL,NULL,"-init_phonon",&init_phonon,NULL);
  PetscOptionsGetInt(NULL,NULL,"-steady_state",&steady_state_solve,NULL);

  if (nid==0) printf("Num_phonon: %ld N_th: %ld num_nv: %ld alpha: %f\n",num_phonon,N_th,num_nv,alpha);
  /* Define scalars to add to Ham */
  w_m        = 475*MHz*2*M_PI; //Mechanical resonator frequency
  gamma_eff  = 145.1*MHz; //Effective dissipation rate
  lambda_s   = 100*1.06*kHz*2*M_PI;
  lambda_s   = 0.1*MHz*2*M_PI;
  lambda_eff = lambda_s*sqrt(alpha)*sqrt(num_nv);
  gamma_par  = 166.666666666*MHz;
  Q          = pow(10,6);  //Mechanical resonator quality factor


  print_dense_ham();

  create_op(num_phonon,&a);
  create_op(2,&nv);

  /* Add terms to the hamiltonian */
  add_to_ham(w_m,a->n); // w_m at a

  add_to_ham(w_m,nv->n); // w_m nvt n

  /* Below 4 terms represent lambda_eff (nvt + nv)(at + a) */
  add_to_ham_mult2(lambda_eff,a->dag,nv->dag); //nvt at
  add_to_ham_mult2(lambda_eff,nv->dag,a);  //nvt a
  add_to_ham_mult2(lambda_eff,nv,a->dag);  //nt at
  add_to_ham_mult2(lambda_eff,nv,a);   //nv a

  /* nv center lindblad terms */
  add_lin(gamma_eff,nv);
  /* add_lin(gamma_par,nv->n); */
  /* add_lin_mult2(gamma_par,nv,nv->dag); */

  /* phonon bath thermal terms */
  rate = w_m/(Q)*(N_th+1);
  add_lin(rate,a);

  rate = w_m/(Q)*(N_th);
  add_lin(rate,a->dag);
  create_full_dm(&rho);

  /* Open file that we will print to in ts_monitor */
  if (nid==0){
    f_pop = fopen("pop","w");
    fprintf(f_pop,"#Time Populations\n");
  }

  if (steady_state_solve==1) {
    steady_state(rho);
  } else {
    set_ts_monitor(ts_monitor);
    set_initial_pop(a,init_phonon);
    set_initial_pop(nv,1);
    set_dm_from_initial_pop(rho);
    time_max  = 100;
    dt        = 1;
    steps_max = 10000;
    time_step(rho,0.0,time_max,dt,steps_max);
  }

  destroy_op(&a);
  destroy_op(&nv);
  destroy_dm(rho);

  QuaC_finalize();
  return 0;
}
PetscErrorCode ts_monitor(TS ts,PetscInt step,PetscReal time,Vec dm,void *ctx){
  double *populations;
  int num_pop,i;
  num_pop = get_num_populations();
  populations = malloc(num_pop*sizeof(double));
  get_populations(dm,&populations);

  if (nid==0){
    /* Print populations to file */
    fprintf(f_pop,"%e",time);
    for(i=0;i<num_pop;i++){
      fprintf(f_pop," %e ",populations[i]);
    }
    fprintf(f_pop,"\n");
  }
  free(populations);
  PetscFunctionReturn(0);

}
