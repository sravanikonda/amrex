
#include "myfunc.H"
#include "myfunc_F.H"
#include <AMReX_BCUtil.H>
#include <AMReX_MultiFabUtil.H>

void SDC_advance(MultiFab& phi_old,
		 MultiFab& phi_new,
		 std::array<MultiFab, AMREX_SPACEDIM>& flux,
		 Real dt,
		 const Geometry& geom,
		 const Vector<BCRec>& bc,
		 MLMG&  mlmg,
		 MLABecLaplacian& mlabec,
		 SDCstruct &SDC, Real a, Real d, Real r)
{

  /*  This is a multi-implicit SDC example time step for an 
      advection-diffusion-reaction equation of the form

      phi_t = A(phi)+D(phi)+R(phi)
      
      The advection is treated explicilty and the diffusion and reaction implicitly
      and uncoupled

      the constants a,d, and r control the strength of each term
  */
  Real qij;

  const BoxArray &ba=phi_old.boxArray();
  const DistributionMapping &dm=phi_old.DistributionMap();

  // Copy old phi into first SDC node
  MultiFab::Copy(SDC.sol[0],phi_old, 0, 0, 1, 2);

  // Fill the ghost cells of each grid from the other grids
  // includes periodic domain boundaries
  SDC.sol[0].FillBoundary(geom.periodicity());
  
  // Fill non-periodic physical boundaries
  FillDomainBoundary(SDC.sol[0], geom, bc);
  
  //  Compute the first function value
  int sdc_m=0;
  SDC_feval(flux,geom,bc,SDC,a,d,r,sdc_m,-1);

  // Copy first function value to all nodes
  for (int sdc_n = 1; sdc_n < SDC.Nnodes; sdc_n++)
    {
      MultiFab::Copy(SDC.f[0][sdc_n],SDC.f[0][0], 0, 0, 1, 0);
      MultiFab::Copy(SDC.f[1][sdc_n],SDC.f[1][0], 0, 0, 1, 0);
      if (SDC.Npieces==3)
	MultiFab::Copy(SDC.f[2][sdc_n],SDC.f[2][0], 0, 0, 1, 0);      
    }


  //  Now do the actual sweeps
  for (int k=1; k <= SDC.Nsweeps; ++k)
    {
      amrex::Print() << "sweep " << k << "\n";

      //  Compute RHS integrals
      SDC.SDC_rhs_integrals(dt);
      
      //  Substep over SDC nodes
      for (int sdc_m = 0; sdc_m < SDC.Nnodes-1; sdc_m++)
	{
	  // use phi_new as rhs and fill it with terms at this iteration
	  SDC.SDC_rhs_k_plus_one(phi_new,dt,sdc_m);
	  
	  // get the best initial guess for implicit solve
	  MultiFab::Copy(SDC.sol[sdc_m+1],phi_new, 0, 0, 1, 2);
	  for ( MFIter mfi(SDC.sol[sdc_m+1]); mfi.isValid(); ++mfi )
	    {
	      //	      const Box& bx = mfi.validbox();
	      qij = dt*SDC.Qimp[sdc_m][sdc_m+1];
	      SDC.sol[sdc_m+1][mfi].saxpy(qij,SDC.f[1][sdc_m+1][mfi]);
	    }

	  // Solve for the first implicit part
	  SDC_fcomp(phi_new, geom, bc, SDC, mlmg, mlabec,dt,d,r,sdc_m+1,1);

	  if (SDC.Npieces==3)
	    {
	      // Build rhs for 2nd solve
	      MultiFab::Copy(phi_new, SDC.sol[sdc_m+1],0, 0, 1, 2);
	      
	      // Add in the part for the 2nd implicit term to rhs
	      SDC.SDC_rhs_misdc(phi_new,dt,sdc_m);
	      
	      // Solve for the second implicit part
	      SDC_fcomp(phi_new, geom, bc, SDC, mlmg, mlabec,dt,d,r,sdc_m+1,2);
	    }
	  // Compute the function values at node sdc_m+1
	  SDC_feval(flux,geom,bc,SDC,a,d,r,sdc_m+1,-1);	  

	} // end SDC substep loop
    }  // end sweeps loop
    
  // Return the last node in phi_new
  MultiFab::Copy(phi_new, SDC.sol[SDC.Nnodes-1], 0, 0, 1, 2);

}

void SDC_feval(std::array<MultiFab, AMREX_SPACEDIM>& flux,
	       const Geometry& geom,
	       const Vector<BCRec>& bc,
	       SDCstruct &SDC,
	       Real a,Real d,Real r,
	       int sdc_m,int npiece)
{
  /*  Evaluate explicitly the rhs terms of the equation at the SDC node "sdc_m".
      The input parameter "npiece" describes which term to do.  
      If npiece = -1, do all the pieces */
  const BoxArray &ba=SDC.sol[0].boxArray();
  const DistributionMapping &dm=SDC.sol[0].DistributionMap();

  const Box& domain_bx = geom.Domain();
  const Real* dx = geom.CellSize();
  int nlo,nhi;
  if (npiece < 0)
    {
      nlo=0;
      nhi=SDC.Npieces;
    }
  else
    {
      nlo=npiece;
      nhi=npiece+1;
    }

  SDC.sol[sdc_m].FillBoundary(geom.periodicity());
  for ( MFIter mfi(SDC.sol[sdc_m]); mfi.isValid(); ++mfi )
    {
      const Box& bx = mfi.validbox();
      for (int n = nlo; n < nhi; n++)    
	{
	  SDC_feval_F(BL_TO_FORTRAN_BOX(bx),
		      BL_TO_FORTRAN_BOX(domain_bx),
		      BL_TO_FORTRAN_ANYD(SDC.sol[sdc_m][mfi]),
		      BL_TO_FORTRAN_ANYD(flux[0][mfi]),
		      BL_TO_FORTRAN_ANYD(flux[1][mfi]),
#if (AMREX_SPACEDIM == 3)   
		      BL_TO_FORTRAN_ANYD(flux[2][mfi]),
#endif		       
		      BL_TO_FORTRAN_ANYD(SDC.f[n][sdc_m][mfi]),
		      dx,&a,&d,&r,&n);
	}
      
    }
}
void SDC_fcomp(MultiFab& rhs,
	       const Geometry& geom,
	       const Vector<BCRec>& bc,
	       SDCstruct &SDC,
	       MLMG &mlmg,
	       MLABecLaplacian& mlabec,	      
	       Real dt,Real d,Real r,
	       int sdc_m,int npiece)
{
  /*  Solve implicitly for the implicit terms of the equation at the SDC node "sdc_m".
      The input parameter "npiece" describes which term to do.  */

  const BoxArray &ba=SDC.sol[0].boxArray();
  const DistributionMapping &dm=SDC.sol[0].DistributionMap();

  const Box& domain_bx = geom.Domain();
  const Real* dx = geom.CellSize();
  Real qij;

    // relative and absolute tolerances for linear solve
  const Real tol_rel = 1.e-12;
  const Real tol_abs = 0.0;
  const Real tol_res = 1.e-10;    // Tolerance on residual
  Real resnorm = 1.e10;    // Tolerance on residual  

  // Make some space for iteration stuff
  MultiFab corr(ba, dm, 1, 2);
  MultiFab resid(ba, dm, 1, 2);  
  if (npiece == 1)  
    {
      // Do diffusion solve

      
      // Fill the ghost cells of each grid from the other grids
      // includes periodic domain boundaries
      rhs.FillBoundary(geom.periodicity());
      SDC.sol[sdc_m].FillBoundary(geom.periodicity());      
      
      // Fill non-periodic physical boundaries
      FillDomainBoundary(rhs, geom, bc);
      FillDomainBoundary(SDC.sol[sdc_m], geom, bc);

      //  Set diffusion scalar in solve
      qij = d*dt*SDC.Qimp[sdc_m-1][sdc_m];	      
      Real ascalar = 1.0;
      mlabec.setScalars(ascalar, qij);

      // set the boundary conditions
      mlabec.setLevelBC(0, &rhs);
      mlabec.setLevelBC(0, &SDC.sol[sdc_m]);
      int resk=0;
      int maxresk=10;
      while ((resnorm > tol_res) & (resk <=maxresk))
	{
	  // Compute residual
	  for ( MFIter mfi(SDC.sol[sdc_m]); mfi.isValid(); ++mfi )
	    {
	      const Box& bx = mfi.validbox();
	      SDC_Lresid_F(BL_TO_FORTRAN_BOX(bx),
			   BL_TO_FORTRAN_BOX(domain_bx),
			   BL_TO_FORTRAN_ANYD(SDC.sol[sdc_m][mfi]),
			   BL_TO_FORTRAN_ANYD(rhs[mfi]),		      
			   BL_TO_FORTRAN_ANYD(resid[mfi]),
			   BL_TO_FORTRAN_ANYD(corr[mfi]),		       
			   &qij,dx); 
	    }
	  resnorm=resid.norm0();
	  ++resk;

	  amrex::Print() << "iter " << resk << ",  residual norm " << resnorm << "\n";	  
	  // includes periodic domain boundaries
	  resid.FillBoundary(geom.periodicity());
	  
	  // Fill non-periodic physical boundaries
	  FillDomainBoundary(resid, geom, bc);
	  
	  //  Do the multigrid solve
	  //mlmg.solve({&SDC.sol[sdc_m]}, {&rhs}, tol_rel, tol_abs);
	  //MultiFab::Copy(corr,SDC.sol[sdc_m], 0, 0, 1, 0);      
	  // set the boundary conditions
	  mlabec.setLevelBC(0, &corr);
	  mlabec.setLevelBC(0, &resid);
	  mlmg.setFixedIter(3);	  
	  mlmg.solve({&corr}, {&resid}, tol_rel, tol_abs);
	  for ( MFIter mfi(SDC.sol[sdc_m]); mfi.isValid(); ++mfi )
	    SDC.sol[sdc_m][mfi].saxpy(1.0,corr[mfi]);  //  make this add
	  
	  // includes periodic domain boundaries
	  SDC.sol[sdc_m].FillBoundary(geom.periodicity());
	  
	  // Fill non-periodic physical boundaries
	  FillDomainBoundary(SDC.sol[sdc_m], geom, bc);
	}
      
    }
  else
    {  // Do reaction solve  y - qij*y*(1-y)*(y-1/2) = rhs

      //  make a flag to change how the reaction is done
      int nflag=1;  // Lazy approximation

      qij = r*dt*SDC.Qimp[sdc_m-1][sdc_m];	            
      for ( MFIter mfi(SDC.sol[sdc_m]); mfi.isValid(); ++mfi )
	{
	  const Box& bx = mfi.validbox();
	  SDC_fcomp_reaction_F(BL_TO_FORTRAN_BOX(bx),
			       BL_TO_FORTRAN_BOX(domain_bx),
			       BL_TO_FORTRAN_ANYD(SDC.sol[sdc_m][mfi]),
			       BL_TO_FORTRAN_ANYD(rhs[mfi]),		      
			       BL_TO_FORTRAN_ANYD(SDC.f[2][sdc_m][mfi]),
			       &qij,&nflag); 
      }
      
    }

}



// OLD CODE COMMENTED OUT
/*
 // Fill the ghost cells of each grid from the other grids
 // includes periodic domain boundaries
 rhs.FillBoundary(geom.periodicity());
 SDC.sol[sdc_m].FillBoundary(geom.periodicity());
 
 // Fill non-periodic physical boundaries
 FillDomainBoundary(rhs, geom, bc);
 FillDomainBoundary(SDC.sol[sdc_m], geom, bc);
 
 //  Set diffusion scalar in solve
 qij = d*dt*SDC.Qimp[sdc_m-1][sdc_m];
 Real ascalar = 1.0;
 mlabec.setScalars(ascalar, qij);
 
 // set the boundary conditions
 mlabec.setLevelBC(0, &rhs);
 mlabec.setLevelBC(0, &SDC.sol[sdc_m]);
 int resk=0;
 int maxresk=10;
 while ((resnorm > tol_res) & (resk <=maxresk))
 {
 // Compute residual
 for ( MFIter mfi(SDC.sol[sdc_m]); mfi.isValid(); ++mfi )
 {
 const Box& bx = mfi.validbox();
 SDC_Lresid_F(BL_TO_FORTRAN_BOX(bx),
 BL_TO_FORTRAN_BOX(domain_bx),
 BL_TO_FORTRAN_ANYD(SDC.sol[sdc_m][mfi]),
 BL_TO_FORTRAN_ANYD(rhs[mfi]),
 BL_TO_FORTRAN_ANYD(resid[mfi]),
 BL_TO_FORTRAN_ANYD(corr[mfi]),
 &qij,dx);
 }
 
 //////////////////
 
 ///Code for residual the way I do it.
 // Compute residual
 for ( MFIter mfi(SDC.sol[sdc_m]); mfi.isValid(); ++mfi )
 {
 const Box& bx = mfi.validbox();
 SDC_feval_F(BL_TO_FORTRAN_BOX(bx),
 BL_TO_FORTRAN_BOX(domain_bx),
 BL_TO_FORTRAN_ANYD(SDC.sol[sdc_m][mfi]),
 BL_TO_FORTRAN_ANYD(flux[0][mfi]),
 BL_TO_FORTRAN_ANYD(flux[1][mfi]),
 #if (AMREX_SPACEDIM == 3)
 BL_TO_FORTRAN_ANYD(flux[2][mfi]),
 #endif
 BL_TO_FORTRAN_ANYD(eval_storage[mfi]),
 dx,&a,&d,&r,
 BL_TO_FORTRAN_ANYD(face_bcoef[0][mfi]),
 BL_TO_FORTRAN_ANYD(face_bcoef[1][mfi]),
 BL_TO_FORTRAN_ANYD(prod_stor[0][mfi]),
 BL_TO_FORTRAN_ANYD(prod_stor[1][mfi]),
 &npiece);
 
 }
 //Rescale resid as it is currently just an evaluation
 qijalt = dt*SDC.Qimp[sdc_m-1][sdc_m];
 
 temp_resid.setVal(0.0);
 MultiFab::Saxpy(temp_resid,qijalt,eval_storage,0,0,1,0);
 MultiFab::Saxpy(temp_resid,1.0,rhs,0,0,1,0);
 MultiFab::Saxpy(temp_resid,-1.0,SDC.sol[sdc_m],0,0,1,0);
 
 
 //    MultiFab::Copy(temp_fab,temp_resid,0,0,1,0);
 //    MultiFab::Saxpy(temp_fab,-1.0,resid,0,0,1,0);
 
 //    corrnorm=temp_fab.norm0();
 //    amrex::Print() << "iter " << resk << ",  resid-resid norm " << corrnorm << "\n";
 
 
 temp_corr.setVal(0.0);
 temp_resid.FillBoundary(geom.periodicity());
 
 // Fill non-periodic physical boundaries
 FillDomainBoundary(temp_resid, geom, bc);
 
 
 ////////////////
 resnorm=temp_resid.norm0();
 //      resnorm=resid.norm0();
 //corrnorm=corr.norm0();
 ++resk;
 //amrex::Print() << "iter " << resk << ",  lap norm " << corrnorm << "\n";
 amrex::Print() << "iter " << resk << ",  residual norm " << resnorm << "\n";
 // includes periodic domain boundaries
 //        resid.FillBoundary(geom.periodicity());
 
 // Fill non-periodic physical boundaries
 //        FillDomainBoundary(resid, geom, bc);
 //        corr.setVal(0.0);
 
 //  Do the multigrid solve
 //mlmg.solve({&SDC.sol[sdc_m]}, {&rhs}, tol_rel, tol_abs);
 //MultiFab::Copy(corr,SDC.sol[sdc_m], 0, 0, 1, 0);
 // set the boundary conditions
 //       mlabec.setLevelBC(0, &corr);
 //       mlabec.setLevelBC(0, &resid);
 //       mlmg.setFixedIter(3);
 //       mlmg.solve({&corr}, {&resid}, tol_rel, tol_abs);
 //   for ( MFIter mfi(SDC.sol[sdc_m]); mfi.isValid(); ++mfi )
 //       SDC.sol[sdc_m][mfi].saxpy(1.0,corr[mfi]);  //  make this add
 
 ////////////////////
 mlabec.setLevelBC(0, &temp_corr);
 mlabec.setLevelBC(0, &temp_resid);
 mlmg.setFixedIter(3);
 mlmg.solve({&temp_corr}, {&temp_resid}, tol_rel, tol_abs);
 
 for ( MFIter mfi(SDC.sol[sdc_m]); mfi.isValid(); ++mfi )
 SDC.sol[sdc_m][mfi].saxpy(1.0,temp_corr[mfi]);
 
 //       MultiFab::Saxpy(temp_corr,-1.0,corr,0,0,1,0);
 
 //corrnorm=temp_corr.norm0();
 // amrex::Print() << "iter " << resk << ",  corr-corr norm " << corrnorm << "\n";
 //amrex::Print() << "iter " << resk << "\n";
 ////////////////////
 
 
 
 //corrnorm=corr.norm0();
 //amrex::Print() << "iter " << resk << ",  Correction norm " << corrnorm << "\n";
 
 // includes periodic domain boundaries
 SDC.sol[sdc_m].FillBoundary(geom.periodicity());
 
 // Fill non-periodic physical boundaries
 FillDomainBoundary(SDC.sol[sdc_m], geom, bc);
 
 //corrnorm=SDC.sol[sdc_m].norm0();
 //amrex::Print() << "iter " << resk << ",  solution norm " << corrnorm << "\n";
 }
 
 */
