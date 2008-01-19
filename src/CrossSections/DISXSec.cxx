//____________________________________________________________________________
/*
 Copyright (c) 2003-2008, GENIE Neutrino MC Generator Collaboration
 For the full text of the license visit http://copyright.genie-mc.org
 or see $GENIE/LICENSE

 Author: Costas Andreopoulos <C.V.Andreopoulos@rl.ac.uk>
         STFC, Rutherford Appleton Laboratory - May 04, 2004

 For the class documentation see the corresponding header file.

 Important revisions after version 2.0.0 :
 @ Jan 19, 2008 - CA
   Modify the way knots are distributed in the cached free nucleon DIS cross
   section splines so that the energy threshold is treated more accurately 
   (see also XSecSplineList.cxx).
*/
//____________________________________________________________________________

#include <TMath.h>
#include <TSystem.h>

#include "Algorithm/AlgConfigPool.h"
#include "Conventions/Constants.h"
#include "CrossSections/DISXSec.h"
#include "CrossSections/GXSecFunc.h"
#include "Messenger/Messenger.h"
#include "Numerical/IntegratorI.h"
#include "PDG/PDGCodes.h"
#include "PDG/PDGUtils.h"
#include "Utils/MathUtils.h"
#include "Utils/Range1.h"
#include "Utils/Cache.h"
#include "Utils/CacheBranchFx.h"
#include "Utils/XSecSplineList.h"

using namespace genie;
using namespace genie::constants;

//____________________________________________________________________________
DISXSec::DISXSec() :
XSecIntegratorI("genie::DISXSec")
{

}
//____________________________________________________________________________
DISXSec::DISXSec(string config) :
XSecIntegratorI("genie::DISXSec", config)
{

}
//____________________________________________________________________________
DISXSec::~DISXSec()
{

}
//____________________________________________________________________________
double DISXSec::Integrate(
                 const XSecAlgorithmI * model, const Interaction * in) const
{
  if(! model->ValidProcess(in) ) return 0.;

  const KPhaseSpace & kps = in->PhaseSpace();
  if(!kps.IsAboveThreshold()) {
     LOG("DISXSec", pDEBUG)  << "*** Below energy threshold";
     return 0;
  }

  const InitialState & init_state = in->InitState();
  double Ev = init_state.ProbeE(kRfHitNucRest);

  int nucpdgc = init_state.Tgt().HitNucPdg();
  int NNucl   = (pdg::IsProton(nucpdgc)) ? 
                   init_state.Tgt().Z() : init_state.Tgt().N();
  
  // If the input interaction is off a nuclear target, then chek whether 
  // the corresponding free nucleon cross section already exists at the 
  // cross section spline list. 
  // If yes, calculate the nuclear cross section based on that value.
  //
  XSecSplineList * xsl = XSecSplineList::Instance();
  if(init_state.Tgt().IsNucleus() && !xsl->IsEmpty() ) {
    Interaction * interaction = new Interaction(*in);
    Target * target = interaction->InitStatePtr()->TgtPtr();
    if(pdg::IsProton(nucpdgc)) { target->SetId(kPdgTgtFreeP); }
    else                       { target->SetId(kPdgTgtFreeN); }
    if(xsl->SplineExists(model,interaction)) {
      const Spline * spl = xsl->GetSpline(model, interaction);
      double xsec = spl->Evaluate(Ev);
      LOG("DISXSec", pINFO)  
        << "From XSecSplineList: XSec[DIS,free nucleon] (E = " << Ev << " GeV) = " << xsec;
      if(! interaction->TestBit(kIAssumeFreeNucleon) ) { 
          xsec *= NNucl; 
          LOG("DISXSec", pINFO)  << "XSec[DIS] (E = " << Ev << " GeV) = " << xsec;
      }
      delete interaction;
      return xsec;
    }
    delete interaction;
  }

  // There was no corresponding free nucleon spline saved in XSecSplineList that
  // could be used to speed up this calculation.
  // Check whether local caching of free nucleon cross sections is allowed.
  // If yes, store free nucleon cross sections at a cache branch and use those 
  // at any subsequent call.
  //
  if(!gSystem->Getenv("GDISABLECACHING")) {
     Cache * cache = Cache::Instance();
     Interaction * interaction = new Interaction(*in);
     string key = this->CacheBranchName(model,interaction);
     LOG("DISXSec", pINFO) << "Finding cache branch with key: " << key;
     CacheBranchFx * cache_branch =
           dynamic_cast<CacheBranchFx *> (cache->FindCacheBranch(key));
     if(!cache_branch) {
         this->CacheFreeNucleonXSec(model,interaction);
         cache_branch =
           dynamic_cast<CacheBranchFx *> (cache->FindCacheBranch(key));
         assert(cache_branch);
     }
     const CacheBranchFx & cb = (*cache_branch);
     double xsec = cb(Ev);
     if(! interaction->TestBit(kIAssumeFreeNucleon) ) { xsec *= NNucl; }
     LOG("DISXSec", pINFO)  << "XSec[DIS] (E = " << Ev << " GeV) = " << xsec;
     delete interaction;
     return xsec;
  }
  else {
    // Just go ahead and integrate the input differential cross section for the 
    // specified interaction.
    //
     Interaction * interaction = new Interaction(*in);
     interaction->SetBit(kISkipProcessChk);
//   interaction->SetBit(kISkipKinematicChk);

     // **Important note** 
     // Based on discussions with Hugh at the GENIE mini-workshop / RAL - July '07
     // The DIS nuclear corrections re-distribute the strength in x,y but do not
     // affect the total cross-section They should be disabled at this step.
     // But they should be enabled at the DIS thread's kinematical selection.
     // Since nuclear corrections don't need to be included at this stage, all the
     // nuclear cross sections can be trivially built from the free nucleon ones.
     //
     interaction->SetBit(kINoNuclearCorrection);

     Range1D_t Wl  = kps.WLim();
     Range1D_t Q2l = kps.Q2Lim();
     LOG("DISXSec", pINFO)  
            << "W integration range = [" << Wl.min << ", " << Wl.max << "]";
     LOG("DISXSec", pINFO)  
         << "Q2 integration range = [" << Q2l.min << ", " << Q2l.max << "]";

     GXSecFunc * func = new Integrand_D2XSec_DWDQ2_E(model, interaction);
     func->SetParam(0,"W", Wl);
     func->SetParam(1,"Q2",Q2l);
     double xsec = fIntegrator->Integrate(*func);
/*
     Range1D_t xl = kps.Limits(kKVx);
     Range1D_t yl = kps.Limits(kKVy);
     LOG("DISXSec", pINFO)  
            << "x integration range = [" << xl.min << ", " << xl.max << "]";
     LOG("DISXSec", pINFO)  
            << "y integration range = [" << yl.min << ", " << yl.max << "]";

     GXSecFunc * func = new Integrand_D2XSec_DxDy_E(model, interaction);
     func->SetParam(0,"x",xl);
     func->SetParam(1,"y",yl);
     double xsec = fIntegrator->Integrate(*func);
*/
     LOG("DISXSec", pINFO)  << "XSec[DIS] (E = " << Ev << " GeV) = " << xsec;

     delete interaction;
     delete func;
     return xsec;
  }
  return 0;
}
//____________________________________________________________________________
void DISXSec::Configure(const Registry & config)
{
  Algorithm::Configure(config);
  this->LoadConfig();
}
//____________________________________________________________________________
void DISXSec::Configure(string config)
{
  Algorithm::Configure(config);
  this->LoadConfig();
}
//____________________________________________________________________________
void DISXSec::LoadConfig(void)
{
  //-- get specified integration algorithm
  fIntegrator = dynamic_cast<const IntegratorI *> (this->SubAlg("Integrator"));
  assert(fIntegrator);

  //-- energy range for cached splines
  AlgConfigPool * confp = AlgConfigPool::Instance();
  const Registry * gc = confp->GlobalParameterList();
  fVldEmin = gc->GetDouble("GVLD-Emin");
  fVldEmax = gc->GetDouble("GVLD-Emax");
}
//____________________________________________________________________________
void DISXSec::CacheFreeNucleonXSec(
          const XSecAlgorithmI * model, const Interaction * interaction) const
{
  LOG("DISXSec", pWARN)  
      << "Wait while computing/caching free nucleon DIS xsections first...";

  // Create the cache branch
  Cache * cache = Cache::Instance();
  string key = this->CacheBranchName(model,interaction);
  CacheBranchFx * cache_branch =
           dynamic_cast<CacheBranchFx *> (cache->FindCacheBranch(key));
  assert(!cache_branch);
  cache_branch = new CacheBranchFx("DIS XSec");
  cache->AddCacheBranch(key, cache_branch);

  // Create the integrand
  GXSecFunc * func = new Integrand_D2XSec_DWDQ2_E(model, interaction);

  // Tweak interaction to be on a free nucleon target
  Target * target = interaction->InitStatePtr()->TgtPtr();
  int nucpdgc = target->HitNucPdg();
  if(pdg::IsProton(nucpdgc)) { target->SetId(kPdgTgtFreeP); }
  else                       { target->SetId(kPdgTgtFreeN); }

  // Compute threshold
  const KPhaseSpace & kps = interaction->PhaseSpace();
  double Ethr = kps.Threshold();

  // Compute the number of spline knots - use at least 10 knots per decade 
  // && at least 40 knots in the full energy range
  const double Emin       = fVldEmin/3.; 
  const double Emax       = fVldEmax*3.; 
  const int    nknots_min = (int) (10*(TMath::Log(Emax) - TMath::Log(Emin))); 
  const int    nknots     = TMath::Max(40, nknots_min); 

  // Distribute the knots in the energy range as is being done in the
  // XSecSplineList so that the energy threshold is treated correctly
  // in the spline - see comments there in.
  double * E = new double[nknots]; 
  int nkb = (Ethr>Emin) ? 5 : 0; // number of knots <  threshold
  int nka = nknots-nkb;          // number of knots >= threshold
  // knots < energy threshold
  double dEb =  (Ethr>Emin) ? (Ethr - Emin) / nkb : 0;
  for(int i=0; i<nkb; i++) {
      E[i] = Emin + i*dEb;
  }
  // knots >= energy threshold
  double E0  = TMath::Max(Ethr,Emin);
  double dEa = (TMath::Log10(Emax) - TMath::Log10(E0)) /(nka-1);
  for(int i=0; i<nka; i++) {
      E[i+nkb] = TMath::Power(10., TMath::Log10(E0) + i * dEa);
  }

  // Compute the cross section at the given set of knots
  for(int ie=0; ie<nknots; ie++) {
    double Ev = E[ie];
    TLorentzVector p4(0,0,Ev,Ev);
    interaction->InitStatePtr()->SetProbeP4(p4);
    double xsec = 0.;
    if(Ev>Ethr) {
       Range1D_t Wl  = kps.WLim();
       Range1D_t Q2l = kps.Q2Lim();
       func->SetParam(0,"W", Wl);
       func->SetParam(1,"Q2",Q2l);
       xsec = fIntegrator->Integrate(*func);
    }
    LOG("DISXSec", pNOTICE)  
                << "Caching: XSec[DIS] (E = " << Ev << " GeV) = " << xsec;
    cache_branch->AddValues(Ev,xsec);
  }//ie

  // Create the spline
  cache_branch->CreateSpline();

  delete [] E;
  delete func;
}
//____________________________________________________________________________
string DISXSec::CacheBranchName(
          const XSecAlgorithmI * model, const Interaction * interaction) const
{
// Build a unique name for the cache branch

  Cache * cache = Cache::Instance();
      
  string algkey = model->Id().Key();
  string ikey   = interaction->AsString();  
  string key    = cache->CacheBranchKey(algkey, ikey);
  return key;
}
//____________________________________________________________________________

