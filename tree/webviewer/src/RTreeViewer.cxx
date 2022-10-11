// Author: Sergey Linev, 7.10.2022

/*************************************************************************
 * Copyright (C) 1995-2022, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include <ROOT/RTreeViewer.hxx>

// #include <ROOT/RLogger.hxx>
#include <ROOT/RWebWindow.hxx>

#include "TTree.h"
#include "TVirtualPad.h"
#include "TBranch.h"
#include "TTimer.h"
// #include "THttpServer.h"
#include "TBufferJSON.h"


namespace ROOT {
namespace Experimental {

class TProgressTimer : public TTimer {
   RTreeViewer &fViewer;
public:
   TProgressTimer(RTreeViewer &viewer, Int_t period) : TTimer(period, kTRUE), fViewer(viewer)
   {
   }

   Bool_t Notify() override
   {
      fViewer.SendProgress();
      return kTRUE;
   }
};

} // namespace Experimental
} // namespace ROOT


using namespace ROOT::Experimental;
using namespace std::string_literals;


//////////////////////////////////////////////////////////////////////////////////////////////
/// constructor

RTreeViewer::RTreeViewer(TTree *tree)
{
   fWebWindow = RWebWindow::Create();
   fWebWindow->SetDefaultPage("file:rootui5sys/tree/index.html");

   // this is call-back, invoked when message received via websocket
   fWebWindow->SetConnectCallBack([this](unsigned connid) { WebWindowConnect(connid); });
   fWebWindow->SetDataCallBack([this](unsigned connid, const std::string &arg) { WebWindowCallback(connid, arg); });
   fWebWindow->SetGeometry(900, 700); // configure predefined window geometry
   fWebWindow->SetConnLimit(1); // allow the only connection
   fWebWindow->SetMaxQueueLength(30); // number of allowed entries in the window queue

   if (tree) SetTree(tree);
}

//////////////////////////////////////////////////////////////////////////////////////////////
/// destructor

RTreeViewer::~RTreeViewer()
{

}

//////////////////////////////////////////////////////////////////////////////////////////////
/// assign new TTree to the viewer

void RTreeViewer::SetTree(TTree *tree)
{
   fTree = tree;

   UpdateConfig();

   Update();
}

/////////////////////////////////////////////////////////////////////////////////
/// Show or update viewer in web window
/// If web browser already started - just refresh drawing like "reload" button does
/// If no web window exists or \param always_start_new_browser configured, starts new window
/// \param args arguments to display

void RTreeViewer::Show(const RWebDisplayArgs &args, bool always_start_new_browser)
{
   std::string user_args = "";
   if (!GetShowHierarchy()) user_args = "{ nobrowser: true }";
   fWebWindow->SetUserArgs(user_args);

   if (args.GetWidgetKind().empty())
      const_cast<RWebDisplayArgs *>(&args)->SetWidgetKind("RTreeViewer");

   if ((fWebWindow->NumConnections(true) == 0) || always_start_new_browser)
      fWebWindow->Show(args);
   else
      Update();
}

//////////////////////////////////////////////////////////////////////////////////////////////
/// Return URL address of web window used for tree viewer

std::string RTreeViewer::GetWindowAddr() const
{
   return fWebWindow ? fWebWindow->GetAddr() : ""s;
}

//////////////////////////////////////////////////////////////////////////////////////////////
/// Update tree viewer in all web displays

void RTreeViewer::Update()
{
   SendCfg(0);
}

//////////////////////////////////////////////////////////////////////////////////////////////
/// Send data for initialize viewer

void RTreeViewer::SendCfg(unsigned connid)
{
   std::string json = "CFG:"s + TBufferJSON::ToJSON(&fCfg, TBufferJSON::kSkipTypeInfo + TBufferJSON::kNoSpaces).Data();

   fWebWindow->Send(connid, json);
}


//////////////////////////////////////////////////////////////////////////////////////////////
/// react on new connection

void RTreeViewer::WebWindowConnect(unsigned connid)
{
   SendCfg(connid);
}

//////////////////////////////////////////////////////////////////////////////////////////////
/// receive data from client

void RTreeViewer::WebWindowCallback(unsigned connid, const std::string &arg)
{
   if (arg == "GETCFG"s) {

      SendCfg(connid);

   } else if (arg == "QUIT_ROOT"s) {

      fWebWindow->TerminateROOT();

   } if (arg.compare(0, 5, "DRAW:"s) == 0) {

      InvokeTreeDraw(arg.substr(5));
   }
}

//////////////////////////////////////////////////////////////////////////////////////////////
/// Update RConfig data

void RTreeViewer::UpdateConfig()
{
   fCfg.fBranches.clear();

   if (!fTree) return;

   fCfg.fTreeName = fTree->GetName();

   TIter iter(fTree->GetListOfBranches());

   while (auto br = dynamic_cast<TBranch *>(iter())) {
      fCfg.fBranches.emplace_back(br->GetName(), br->GetTitle());
   }

   fCfg.fTreeEntries = fTree->GetEntries();

   fCfg.fStep = 1;
   fCfg.fLargerStep = fCfg.fTreeEntries/100;
   if (fCfg.fLargerStep < 2) fCfg.fLargerStep = 2;
}


//////////////////////////////////////////////////////////////////////////////////////////////
/// Invoke tree drawing

void RTreeViewer::InvokeTreeDraw(const std::string &json)
{
   auto newcfg = TBufferJSON::FromJSON<RConfig>(json);

   if (!newcfg || !fTree) return;

   fCfg = *newcfg;

   UpdateConfig();

   std::string expr = fCfg.fExprX;
   if (!fCfg.fExprY.empty()) {
      expr += ":"s;
      expr += fCfg.fExprY;

      if (!fCfg.fExprZ.empty()) {
         expr += ":"s;
         expr += fCfg.fExprZ;
      }
   }

   // printf("Draw %s\n", expr.c_str());

   Long64_t nentries = (fCfg.fNumber > 0) ? fCfg.fNumber : TTree::kMaxEntries;

   if (!fProgrTimer)
      fProgrTimer = std::make_unique<TProgressTimer>(*this, 50);

   fTree->SetTimerInterval(50);

   fLastSendProgress.clear();

   // SendProgress();

   fProgrTimer->TurnOn();

   fTree->Draw(expr.c_str(), fCfg.fExprCut.c_str(), fCfg.fOption.c_str(), nentries, fCfg.fFirst);

   fProgrTimer->TurnOff();

   if (!fLastSendProgress.empty())
      SendProgress(true);

   std::string canv_name;

   if (gPad) {
      gPad->Update();
      canv_name = gPad->GetName();
   }

   // at the end invoke callback
   if (fCallback)
      fCallback(canv_name);
}

//////////////////////////////////////////////////////////////////////////////////////////////
/// Send progress to the client

void RTreeViewer::SendProgress(bool completed)
{
   std::string progress = "100";

   if (!completed) {

      Long64_t first = fCfg.fFirst;
      Long64_t nentries = fTree->GetEntries();
      Long64_t last = nentries;
      if ((fCfg.fNumber > 0) && (first + fCfg.fNumber < nentries))
         last = first + fCfg.fNumber;

      Long64_t current = fTree->GetReadEntry();

      if (last > first)
         progress = std::to_string((current - first + 1.) / ( last - first + 0. ) * 100.);
   }

   if (fLastSendProgress == progress)
      return;

   fLastSendProgress = progress;

   // printf("Progress %s\n", progress.c_str());

   fWebWindow->Send(0, "PROGRESS:"s + progress);
}

