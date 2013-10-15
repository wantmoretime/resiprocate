#include "resip/stack/MultipartMixedContents.hxx"
#include "resip/stack/MultipartAlternativeContents.hxx"
#include "resip/dum/Dialog.hxx"
#include "resip/dum/DialogEventStateManager.hxx"
#include "resip/dum/DialogUsageManager.hxx"
#include "resip/dum/DumTimeout.hxx"
#include "resip/dum/InviteSessionHandler.hxx"
#include "resip/dum/ServerInviteSession.hxx"
#include "resip/dum/MasterProfile.hxx"
#include "resip/dum/UsageUseException.hxx"
#include "resip/dum/DumHelper.hxx"
#include "resip/dum/DumCommand.hxx"
#include "rutil/Logger.hxx"
#include "rutil/compat.hxx"
#include "rutil/WinLeakCheck.hxx"

using namespace resip;

#define RESIPROCATE_SUBSYSTEM Subsystem::DUM

ServerInviteSession::ServerInviteSession(DialogUsageManager& dum, Dialog& dialog, const SipMessage& request)
   : InviteSession(dum, dialog),
     mFirstRequest(request),
     m1xx(new SipMessage),
     mCurrentRetransmit1xxSeq(0),
     mLocalRSeq(0),
     mAnswerSentReliably(false)
{
   assert(request.isRequest());
   mState = UAS_Start;
}

ServerInviteSessionHandle 
ServerInviteSession::getHandle()
{
   return ServerInviteSessionHandle(mDum, getBaseHandle().getId());
}

void 
ServerInviteSession::redirect(const NameAddrs& contacts, int code)
{
   InfoLog (<< toData(mState) << ": redirect(" << code << ")"); // -> " << contacts);

   switch (mState)
   {
      case UAS_EarlyNoOffer:
      case UAS_EarlyOffer:
      case UAS_EarlyProvidedAnswer:
      case UAS_EarlyProvidedOffer:
      case UAS_NegotiatedReliable:
      case UAS_FirstSentAnswerReliable:
      case UAS_FirstSentOfferReliable:
      case UAS_NoOffer:
      case UAS_NoOfferReliable:
      case UAS_ProvidedOfferReliable:
      case UAS_OfferReliableProvidedAnswer:
      case UAS_NoAnswerReliableWaitingPrack:
      case UAS_NoAnswerReliable:
      case UAS_Offer:
      case UAS_OfferProvidedAnswer:
      case UAS_OfferReliable: 
      case UAS_ProvidedOffer:
      case UAS_ReceivedUpdate:
      case UAS_SentUpdate:
      {
         // !jf! the cleanup for 3xx may be a bit strange if we are in the middle of
         // an offer/answer exchange with PRACK. 
         // e.g. we sent 183 reliably and then 302 before PRACK was received. Ideally,
         // we should send 200PRACK
         SharedPtr<SipMessage> response(new SipMessage);
         mDialog.makeResponse(*response, mFirstRequest, code);
         response->header(h_Contacts) = contacts;
         send(response);

         if (mDum.mDialogEventStateManager)
         {
            mDum.mDialogEventStateManager->onTerminated(mDialog, *response, InviteSessionHandler::Rejected);
         }

         transition(Terminated);

         mDum.mInviteSessionHandler->onTerminated(getSessionHandle(), InviteSessionHandler::Referred); 
         mDum.destroy(this);
         break;
      }

      case UAS_Accepted:
      case UAS_WaitingToOffer:
      case UAS_WaitingToRequestOffer:
      case UAS_WaitingToHangup:
      case UAS_WaitingToTerminate:
      case UAS_SentUpdateAccepted:
      case UAS_ReceivedUpdateWaitingAnswer:
      case UAS_Start:
      default:
         assert(0);
         throw UsageUseException("Can't redirect after accepted", __FILE__, __LINE__);
         break;
   }
}

class ServerInviteSessionRedirectCommand : public DumCommandAdapter
{
public:
   ServerInviteSessionRedirectCommand(const ServerInviteSessionHandle& serverInviteSessionHandle, const NameAddrs& contacts, int code)
      : mServerInviteSessionHandle(serverInviteSessionHandle),
      mContacts(contacts),
      mCode(code)
   {

   }

   virtual void executeCommand()
   {
      if(mServerInviteSessionHandle.isValid())
      {
         mServerInviteSessionHandle->redirect(mContacts, mCode);
      }
   }

   virtual EncodeStream& encodeBrief(EncodeStream& strm) const
   {
      return strm << "ServerInviteSessionRedirectCommand";
   }
private:
   ServerInviteSessionHandle mServerInviteSessionHandle;
   NameAddrs mContacts;
   int mCode;
};

void 
ServerInviteSession::redirectCommand(const NameAddrs& contacts, int code)
{
   mDum.post(new ServerInviteSessionRedirectCommand(getHandle(), contacts, code));
}

void 
ServerInviteSession::provisional(int code, bool earlyFlag)
{
   InfoLog (<< toData(mState) << ": provisional(" << code << ")");

   switch (mState)
   {
      case UAS_Offer:
         transition(UAS_EarlyOffer);
         sendProvisional(code, earlyFlag);
         break;

      case UAS_OfferProvidedAnswer:
      case UAS_EarlyProvidedAnswer:
         transition(UAS_EarlyProvidedAnswer);
         sendProvisional(code, earlyFlag);
         break;

      case UAS_ProvidedOffer:
      case UAS_EarlyProvidedOffer:
         transition(UAS_EarlyProvidedOffer);
         sendProvisional(code, earlyFlag);
         break;
         
      case UAS_EarlyOffer:
         transition(UAS_EarlyOffer);
         sendProvisional(code, earlyFlag);
         break;
         
      case UAS_NoOffer:
      case UAS_EarlyNoOffer:
         transition(UAS_EarlyNoOffer);
         sendProvisional(code, earlyFlag);
         break;
         
      case UAS_NoOfferReliable:
      case UAS_NegotiatedReliable:
      case UAS_FirstSentAnswerReliable:
      case UAS_NoAnswerReliableWaitingPrack: 
      case UAS_FirstSentOfferReliable:
         if(mUnacknowledgedReliableProvisional.get())
         {
            InfoLog (<< "Waiting for PRACK. queued provisional" );
            queueResponse(code, earlyFlag);
         }
         else
         {
            sendProvisional(code, earlyFlag);
         }
         break;
         
      case UAS_NoAnswerReliable:
      case UAS_OfferReliable: 
         if(sendProvisional(code, earlyFlag))
         {
            // If sent reliably then change state
            transition(UAS_NoAnswerReliableWaitingPrack);
         }
         break;

      case UAS_ProvidedOfferReliable:
         if(sendProvisional(code, earlyFlag))
         {
            // If sent reliably then change state
            transition(UAS_FirstSentOfferReliable);
         }
         break;

      case UAS_OfferReliableProvidedAnswer: 
         if(sendProvisional(code, earlyFlag))
         {
            // If sent reliably then change state
            transition(UAS_FirstSentAnswerReliable);
         }
         break;
         
      case UAS_Accepted:
      case UAS_WaitingToOffer:
      case UAS_WaitingToRequestOffer:
      case UAS_ReceivedUpdate:
      case UAS_ReceivedUpdateWaitingAnswer:
      case UAS_SentUpdate:
      case UAS_SentUpdateAccepted:
      case UAS_Start:
      case UAS_WaitingToHangup:
      case UAS_WaitingToTerminate:
      default:
         assert(0);
         break;
   }
}

class ServerInviteSessionProvisionalCommand : public DumCommandAdapter
{
public:
   ServerInviteSessionProvisionalCommand(const ServerInviteSessionHandle& serverInviteSessionHandle, int statusCode)
      : mServerInviteSessionHandle(serverInviteSessionHandle),
        mStatusCode(statusCode)
   {
   }

   virtual void executeCommand()
   {
      if(mServerInviteSessionHandle.isValid())
      {
         mServerInviteSessionHandle->provisional(mStatusCode);
      }
   }

   virtual EncodeStream& encodeBrief(EncodeStream& strm) const
   {
      return strm << "ServerInviteSessionProvisionalCommand";
   }
private:
   ServerInviteSessionHandle mServerInviteSessionHandle;
   int mStatusCode;
};

void 
ServerInviteSession::provisionalCommand(int statusCode)
{
   mDum.post(new ServerInviteSessionProvisionalCommand(getHandle(), statusCode));
}

void
ServerInviteSession::provideOffer(const Contents& offer,
                                  DialogUsageManager::EncryptionLevel level, 
                                  const Contents* alternative,
                                  bool sendOfferAtAccept)
{
   InfoLog (<< toData(mState) << ": provideOffer");
   mAnswerSentReliably = false;
   switch (mState)
   {
      case UAS_NoOffer:
         transition(UAS_ProvidedOffer);
         mProposedLocalOfferAnswer = InviteSession::makeOfferAnswer(offer, alternative);
         mProposedEncryptionLevel = level;
         break;

      case UAS_EarlyNoOffer:
         transition(UAS_EarlyProvidedOffer);
         mProposedLocalOfferAnswer = InviteSession::makeOfferAnswer(offer, alternative);
         mProposedEncryptionLevel = level;
         break;
         
      case UAS_NoOfferReliable:
         transition(UAS_ProvidedOfferReliable);
         mProposedLocalOfferAnswer = InviteSession::makeOfferAnswer(offer, alternative);
         mProposedEncryptionLevel = level;
         break;

      case UAS_NegotiatedReliable:
         mProposedLocalOfferAnswer = InviteSession::makeOfferAnswer(offer, alternative);
         mProposedEncryptionLevel = level;
         if (sendOfferAtAccept)
         {
            transition(UAS_ProvidedOffer);
         }
         else
         {
            transition(UAS_SentUpdate);
            sendUpdate(offer);
         }
         break;
         
      case UAS_Accepted:
         // queue the offer to be sent after the ACK is received
         transition(UAS_WaitingToOffer);
         mProposedLocalOfferAnswer = InviteSession::makeOfferAnswer(offer);
         mProposedEncryptionLevel = level;
         break;

      case UAS_WaitingToOffer:
         InviteSession::provideOffer(offer, level, alternative);
         break;

      case UAS_EarlyProvidedAnswer:
      case UAS_EarlyProvidedOffer:
      case UAS_FirstSentAnswerReliable:
      case UAS_FirstSentOfferReliable:
      case UAS_Offer:
      case UAS_EarlyOffer:
      case UAS_OfferReliable: 
      case UAS_OfferProvidedAnswer:
      case UAS_ProvidedOffer:
      case UAS_ProvidedOfferReliable:
      case UAS_ReceivedUpdate:
      case UAS_ReceivedUpdateWaitingAnswer:
      case UAS_SentUpdate:
      case UAS_SentUpdateAccepted:
      case UAS_Start:
      case UAS_WaitingToHangup:
      case UAS_WaitingToTerminate:
      case UAS_WaitingToRequestOffer:
      case UAS_AcceptedWaitingAnswer:
         assert(0);
         break;
      default:
         InviteSession::provideOffer(offer, level, alternative);
         break;
   }
}

void 
ServerInviteSession::provideOffer(const Contents& offer,
                                  bool sendOfferAtAccept)
{
   this->provideOffer(offer, mCurrentEncryptionLevel, 0, sendOfferAtAccept);
}


void
ServerInviteSession::requestOffer()
{
   InfoLog (<< toData(mState) << ": requestOffer");
   switch (mState)
   {
      case UAS_Accepted:
         // queue the request to be sent after the ACK is received
         transition(UAS_WaitingToRequestOffer);
         break;

      case UAS_WaitingToRequestOffer:
         InviteSession::requestOffer();
         break;

      default:
         InviteSession::requestOffer();
         break;
   }
}

void 
ServerInviteSession::provideAnswer(const Contents& answer)
{
   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   InfoLog (<< toData(mState) << ": provideAnswer");
   mAnswerSentReliably = false;
   switch (mState)
   {
      case UAS_Offer:
         transition(UAS_OfferProvidedAnswer);
         mCurrentRemoteOfferAnswer = mProposedRemoteOfferAnswer;
         mCurrentLocalOfferAnswer = InviteSession::makeOfferAnswer(answer);
         break;

      case UAS_EarlyOffer:
         transition(UAS_EarlyProvidedAnswer);
         mCurrentRemoteOfferAnswer = mProposedRemoteOfferAnswer;
         mCurrentLocalOfferAnswer = InviteSession::makeOfferAnswer(answer);
         break;
         
      case UAS_OfferReliable: 
         mCurrentRemoteOfferAnswer = mProposedRemoteOfferAnswer;
         mCurrentLocalOfferAnswer = InviteSession::makeOfferAnswer(answer);
         transition(UAS_OfferReliableProvidedAnswer);
         break;

      case UAS_NoAnswerReliableWaitingPrack: 
         // Store answer and wait for PRACK
         mCurrentRemoteOfferAnswer = mProposedRemoteOfferAnswer;
         mCurrentLocalOfferAnswer = InviteSession::makeOfferAnswer(answer);
         break;

      case UAS_ReceivedUpdate:
         {
            transition(UAS_NegotiatedReliable);

            // Send answer in 200/Update
            SharedPtr<SipMessage> response(new SipMessage);
            mDialog.makeResponse(*response, *mLastRemoteSessionModification, 200);
            InviteSession::setOfferAnswer(*response, answer, 0);
            mCurrentLocalOfferAnswer = InviteSession::makeOfferAnswer(answer);
            mCurrentRemoteOfferAnswer = mProposedRemoteOfferAnswer;
            InfoLog (<< "Sending " << response->brief());
            DumHelper::setOutgoingEncryptionLevel(*response, mCurrentEncryptionLevel);
            send(response);
         }
         break;
         
      case UAS_ReceivedUpdateWaitingAnswer:
         {
            SharedPtr<SipMessage> response(new SipMessage);
            mDialog.makeResponse(*response, *mLastRemoteSessionModification, 200);
            InviteSession::setOfferAnswer(*response, answer, 0);
            mCurrentLocalOfferAnswer = InviteSession::makeOfferAnswer(answer);
            mCurrentRemoteOfferAnswer = mProposedRemoteOfferAnswer;
            InfoLog (<< "Sending " << response->brief());
            DumHelper::setOutgoingEncryptionLevel(*response, mCurrentEncryptionLevel);
            send(response);

            // send the queued 200/Invite
            updateCheckQueue();
         }
         break;

      case UAS_NoAnswerReliable:
         mCurrentRemoteOfferAnswer = mProposedRemoteOfferAnswer;
         mCurrentLocalOfferAnswer = InviteSession::makeOfferAnswer(answer);
         transition(UAS_OfferReliableProvidedAnswer);
         break;

      // If we received an offer in a PRACK then we transition to NegotiateReliable and expect
      // provideAnswer to be called to send the 200/Prack containing the answer.  TODO  seems
      // like we really ought to be defining a new state for this
      case UAS_NegotiatedReliable:
         if(mPrackWithOffer.get())
         {
            mCurrentRemoteOfferAnswer = mProposedRemoteOfferAnswer;
            mCurrentLocalOfferAnswer = InviteSession::makeOfferAnswer(answer);
            SharedPtr<SipMessage> p200(new SipMessage);
            mDialog.makeResponse(*p200, *mPrackWithOffer, 200);
            setOfferAnswer(*p200, mCurrentLocalOfferAnswer.get());
            mAnswerSentReliably = true;
            mPrackWithOffer.reset();
            send(p200);
         }
         else
         {
             assert(0);
         }
         break;

      case UAS_Accepted:
      case UAS_WaitingToOffer:
      case UAS_WaitingToRequestOffer:
      case UAS_EarlyNoOffer:
      case UAS_EarlyProvidedAnswer:
      case UAS_EarlyProvidedOffer:
      case UAS_FirstSentAnswerReliable:
      case UAS_FirstSentOfferReliable:
      case UAS_NoOffer:
      case UAS_NoOfferReliable:
      case UAS_OfferProvidedAnswer:
      case UAS_ProvidedOffer:
      case UAS_ProvidedOfferReliable:
      case UAS_SentUpdate:
      case UAS_SentUpdateAccepted:
      case UAS_Start:
      case UAS_WaitingToHangup:
      case UAS_WaitingToTerminate:
      case UAS_AcceptedWaitingAnswer:
         assert(0);
         break;
      default:
         InviteSession::provideAnswer(answer);
         break;
   }
}

void
ServerInviteSession::end()
{
   end(NotSpecified);
}

void
ServerInviteSession::end(const Data& userReason)
{
   mUserEndReason = userReason;
   end(InviteSession::UserSpecified);
}

void 
ServerInviteSession::end(EndReason reason)
{
   InfoLog (<< toData(mState) << ": end");
   if (mEndReason == NotSpecified)
   {
      mEndReason = reason;   
   }
   
   switch (mState)
   {
      case UAS_EarlyNoOffer:
      case UAS_EarlyOffer:
      case UAS_EarlyProvidedAnswer:
      case UAS_EarlyProvidedOffer:
      case UAS_NoOffer:
      case UAS_Offer:
      case UAS_OfferProvidedAnswer:
      case UAS_ProvidedOffer:
      case UAS_ProvidedOfferReliable:
         reject(480);
         break;         
         
      case UAS_OfferReliable: 
      case UAS_OfferReliableProvidedAnswer:
      case UAS_NegotiatedReliable:
      case UAS_FirstSentOfferReliable:
      case UAS_FirstSentAnswerReliable:
      case UAS_NoAnswerReliableWaitingPrack:
      case UAS_NoAnswerReliable:
      case UAS_NoOfferReliable:
      case UAS_ReceivedUpdate:   // !slg! todo: send 488U
      case UAS_ReceivedUpdateWaitingAnswer: // !slg! todo: send 488U
      case UAS_SentUpdate:
      case UAS_SentUpdateGlare:
      case UAS_SentUpdateAccepted:
      case UAS_WaitingToTerminate:
         reject(480);
         break;
         
      case UAS_Start:
         assert(0);
         break;

      case UAS_Accepted:
      case UAS_WaitingToOffer:
      case UAS_WaitingToRequestOffer:
      case UAS_AcceptedWaitingAnswer:
         if(mCurrentRetransmit200)  // If retransmit200 timer is active then ACK is not received yet - wait for it
         {
            transition(UAS_WaitingToHangup);
         }
         else
         {
             // ACK has likely timed out - hangup immediately
             SharedPtr<SipMessage> msg = sendBye();
             transition(Terminated);
             mDum.mInviteSessionHandler->onTerminated(getSessionHandle(), InviteSessionHandler::LocalBye, msg.get());
         }
         break;

      case UAS_WaitingToHangup:     // This can happen if we are waiting for an ACK to hangup and the ACK timesout
          break;
         
      default:
         InviteSession::end(reason);
         break;
   }
}

void 
ServerInviteSession::reject(int code, WarningCategory *warning)
{
   InfoLog (<< toData(mState) << ": reject(" << code << ")");

   switch (mState)
   {
      case UAS_EarlyNoOffer:
      case UAS_EarlyOffer:
      case UAS_EarlyProvidedAnswer:
      case UAS_EarlyProvidedOffer:
      case UAS_NoOffer:
      case UAS_Offer:
      case UAS_OfferProvidedAnswer:
      case UAS_ProvidedOffer:
      case UAS_ProvidedOfferReliable:

      case UAS_AcceptedWaitingAnswer:
      case UAS_NegotiatedReliable:
      case UAS_FirstSentAnswerReliable:
      case UAS_NoAnswerReliableWaitingPrack:
      case UAS_NoAnswerReliable:
      case UAS_FirstSentOfferReliable:
      case UAS_NoOfferReliable:
      case UAS_OfferReliable: 
      case UAS_OfferReliableProvidedAnswer:
      case UAS_ReceivedUpdate:
      case UAS_SentUpdate:
      case UAS_SentUpdateGlare:
      {
         // !jf! the cleanup for 3xx may be a bit strange if we are in the middle of
         // an offer/answer exchange with PRACK. 
         // e.g. we sent 183 reliably and then 302 before PRACK was received. Ideally,
         // we should send 200PRACK
         SharedPtr<SipMessage> response(new SipMessage);
         mDialog.makeResponse(*response, mFirstRequest, code);
         if(warning)
         {
            response->header(h_Warnings).push_back(*warning);
         }
         send(response);

         transition(Terminated);
         mDum.mInviteSessionHandler->onTerminated(getSessionHandle(), InviteSessionHandler::Rejected); 
         mDum.destroy(this);
         break;
      }

      case UAS_Accepted:
      case UAS_WaitingToOffer:
      case UAS_WaitingToRequestOffer:
      case UAS_ReceivedUpdateWaitingAnswer:
      case UAS_SentUpdateAccepted:
      case UAS_Start:
      case UAS_WaitingToHangup:
      case UAS_WaitingToTerminate:
         assert(0);
         break;

      default:
         InviteSession::reject(code);
         break;
   }
}

void 
ServerInviteSession::accept(int code)
{
   InfoLog (<< toData(mState) << ": accept(" << code << ")");
   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   switch (mState)
   {
      case UAS_Offer:
      case UAS_EarlyOffer:
      case UAS_FirstSentOfferReliable:
         assert(0);
         break;

      case UAS_OfferProvidedAnswer:
      case UAS_EarlyProvidedAnswer:
         transition(UAS_Accepted);
         sendAccept(code, mCurrentLocalOfferAnswer.get());
         handler->onConnected(getSessionHandle(), *mInvite200);
         break;
         
      case UAS_NoOffer:
      case UAS_EarlyNoOffer:
         assert(0);
         break;

      case UAS_ProvidedOffer:
      case UAS_ProvidedOfferReliable:
      case UAS_EarlyProvidedOffer:
         transition(UAS_AcceptedWaitingAnswer);
         sendAccept(code, mProposedLocalOfferAnswer.get());
         break;
         
      case UAS_Accepted:
      case UAS_WaitingToOffer:
      case UAS_WaitingToRequestOffer:
         assert(0);  // Already Accepted
         break;
         
      case UAS_FirstSentAnswerReliable:
      case UAS_NoAnswerReliableWaitingPrack:
         // queue 2xx
         // waiting for PRACK
         InfoLog (<< "Waiting for PRACK. queued 200 OK" );
         queueResponse(code, false);
         break;
         
      case UAS_NegotiatedReliable:
         if(mUnacknowledgedReliableProvisional.get())
         {
            InfoLog (<< "Waiting for PRACK. queued provisional" );
            queueResponse(code, false);
         }
         else
         {
            transition(UAS_Accepted);
            sendAccept(code, 0);
            handler->onConnected(getSessionHandle(), *mInvite200);
         }
         break;

      case UAS_OfferReliableProvidedAnswer: 
         transition(UAS_Accepted);
         sendAccept(code, mCurrentLocalOfferAnswer.get());
         handler->onConnected(getSessionHandle(), *mInvite200);
         break;

      case UAS_SentUpdate:
         transition(UAS_SentUpdateAccepted);
         queueResponse(code, false);
         break;

      case UAS_ReceivedUpdate:
         transition(UAS_ReceivedUpdateWaitingAnswer);
         queueResponse(code, false);
         break;
         
      case UAS_NoAnswerReliable:
      case UAS_NoOfferReliable:
      case UAS_OfferReliable: 
      case UAS_ReceivedUpdateWaitingAnswer:
      case UAS_SentUpdateAccepted:
      case UAS_Start:
      case UAS_WaitingToHangup:
      case UAS_WaitingToTerminate:
      default:
         assert(0);
         break;
   }
}

class ServerInviteSessionAcceptCommand : public DumCommandAdapter
{
public:
   ServerInviteSessionAcceptCommand(const ServerInviteSessionHandle& serverInviteSessionHandle, int statusCode)
      : mServerInviteSessionHandle(serverInviteSessionHandle),
        mStatusCode(statusCode)
   {
   }

   virtual void executeCommand()
   {
      if(mServerInviteSessionHandle.isValid())
      {
         mServerInviteSessionHandle->accept(mStatusCode);
      }
   }

   virtual EncodeStream& encodeBrief(EncodeStream& strm) const
   {
      return strm << "ServerInviteSessionAcceptCommand";
   }
private:
   ServerInviteSessionHandle mServerInviteSessionHandle;
   int mStatusCode;
};

void 
ServerInviteSession::acceptCommand(int statusCode)
{
   mDum.post(new ServerInviteSessionAcceptCommand(getHandle(), statusCode));
}

void 
ServerInviteSession::dispatch(const SipMessage& msg)
{
   if (msg.isRequest())
   {
      if (msg.header(h_RequestLine).method() == INFO)
      {
         InviteSession::dispatchInfo(msg);
         return;
      }
      if (msg.header(h_RequestLine).method() == MESSAGE)
      {
         InviteSession::dispatchMessage(msg);
         return;
      }
   }

   switch (mState)
   {
      case UAS_Start:
         dispatchStart(msg);
         break;

      case UAS_Offer:
      case UAS_EarlyOffer:
      case UAS_EarlyProvidedAnswer:
      case UAS_NoOffer:
      case UAS_ProvidedOffer:
      case UAS_ProvidedOfferReliable:
      case UAS_EarlyNoOffer:
      case UAS_EarlyProvidedOffer:
      case UAS_OfferProvidedAnswer:
      case UAS_NoOfferReliable:
      case UAS_OfferReliable:
      case UAS_NoAnswerReliable:
      case UAS_OfferReliableProvidedAnswer: 
         dispatchOfferOrEarly(msg);
         break;       
      case UAS_Accepted:
         dispatchAccepted(msg);
         break;
      case UAS_WaitingToOffer:
         dispatchWaitingToOffer(msg);
         break;
      case UAS_WaitingToRequestOffer:
         dispatchWaitingToRequestOffer(msg);
         break;
      case UAS_AcceptedWaitingAnswer:
         dispatchAcceptedWaitingAnswer(msg);
         break;         
      case UAS_FirstSentAnswerReliable:
         dispatchFirstSentAnswerReliable(msg);
         break;
      case UAS_NoAnswerReliableWaitingPrack:
         dispatchNoAnswerReliableWaitingPrack(msg);
         break;
      case UAS_FirstSentOfferReliable:
         dispatchFirstSentOfferReliable(msg);
         break;
      case UAS_ReceivedUpdate:
         dispatchReceivedUpdate(msg);
         break;
      case UAS_ReceivedUpdateWaitingAnswer:
         dispatchReceivedUpdateWaitingAnswer(msg);
         break;
      case UAS_SentUpdate:
         dispatchSentUpdate(msg);
         break;
      case UAS_WaitingToHangup:
         dispatchWaitingToHangup(msg);
         break;
      case UAS_WaitingToTerminate:
         dispatchWaitingToTerminate(msg);
         break;
      case UAS_SentUpdateAccepted:
         dispatchSentUpdateAccepted(msg);
         break;
      case UAS_NegotiatedReliable:
         dispatchNegotiatedReliable(msg);
         break;
      default:
         InviteSession::dispatch(msg);
         break;
   }
}

void 
ServerInviteSession::dispatch(const DumTimeout& timeout)
{
   if (timeout.type() == DumTimeout::Retransmit1xx)
   {
      if (timeout.seq() == mCurrentRetransmit1xxSeq)  // If timer isn't stopped and this timer is for last 1xx sent, then resend
      {
         send(m1xx);
         startRetransmit1xxTimer();
      }
   }
   else if (timeout.type() == DumTimeout::Retransmit1xxRel)
   {
      if (mUnacknowledgedReliableProvisional.get() &&  
          mUnacknowledgedReliableProvisional->header(h_RSeq).value() == timeout.seq())
      {
         unsigned int duration = 2*timeout.secondarySeq();
         if(duration>=64*Timer::T1)
         {
            InfoLog (<< "Reliable provisional timeout" );
            SharedPtr<SipMessage> i504(new SipMessage);
            mDialog.makeResponse(*i504, mFirstRequest, 504);
            send(i504);
 
            transition(Terminated);
 
            if (mDum.mDialogEventStateManager)
            {
               SipMessage msg;  // should onTerminated require this msg?
               mDum.mDialogEventStateManager->onTerminated(mDialog, msg, InviteSessionHandler::Timeout);
            }
 
            mDum.mInviteSessionHandler->onTerminated(getSessionHandle(), InviteSessionHandler::Timeout);
            mDum.destroy(this);
            return;
         }
         else
         {
            InfoLog (<< "Reliable provisional retransmit" );
            send(mUnacknowledgedReliableProvisional);
            mDum.addTimerMs(DumTimeout::Retransmit1xxRel, duration, getBaseHandle(), timeout.seq(), duration);
         }
      }
   }
   else if (timeout.type() == DumTimeout::Glare)
   {
      if (mState == UAS_SentUpdateGlare)
      {
         transition(UAS_SentUpdate);
         InfoLog (<< "Retransmitting the UPDATE (glare condition timer)");
         mDialog.makeRequest(*mLastLocalSessionModification, UPDATE);  // increments CSeq
         send(mLastLocalSessionModification);
      }
   }
   else
   {
      InviteSession::dispatch(timeout);
   }
}

void
ServerInviteSession::dispatchStart(const SipMessage& msg)
{
   assert(msg.isRequest());
   assert(msg.header(h_CSeq).method() == INVITE);

   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);
   storePeerCapabilities(msg);

   if (mDum.mDialogEventStateManager)
   {
      mDum.mDialogEventStateManager->onTryingUas(mDialog, msg);
   }

   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnInviteOffer:
         *mLastRemoteSessionModification = msg;
         transition(UAS_Offer);
         mProposedRemoteOfferAnswer = InviteSession::makeOfferAnswer(*offerAnswer);
         mCurrentEncryptionLevel = getEncryptionLevel(msg);
         handler->onNewSession(getHandle(), InviteSession::Offer, msg);
         if(!isTerminated())  
         {
            handler->onOffer(getSessionHandle(), msg, *offerAnswer);
         }
         break;
      case OnInvite:
         *mLastRemoteSessionModification = msg;
         transition(UAS_NoOffer);
         handler->onNewSession(getHandle(), InviteSession::None, msg);
         if(!isTerminated())  
         {
            handler->onOfferRequired(getSessionHandle(), msg);
         }
         break;
      case OnInviteReliableOffer:
         *mLastRemoteSessionModification = msg;
         transition(UAS_OfferReliable);
         mProposedRemoteOfferAnswer = InviteSession::makeOfferAnswer(*offerAnswer);
         mCurrentEncryptionLevel = getEncryptionLevel(msg);
         handler->onNewSession(getHandle(), InviteSession::Offer, msg);
         if(!isTerminated())  
         {
            handler->onOffer(getSessionHandle(), msg, *offerAnswer);
         }
         break;
      case OnInviteReliable:
         *mLastRemoteSessionModification = msg;
         transition(UAS_NoOfferReliable);
         handler->onNewSession(getHandle(), InviteSession::None, msg);
         if(!isTerminated())  
         {
            handler->onOfferRequired(getSessionHandle(), msg);
         }
         break;
      default:
         assert(0);
         break;
   }
}

// Offer, Early, EarlyNoOffer, NoOffer
void
ServerInviteSession::dispatchOfferOrEarly(const SipMessage& msg)
{
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);
   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnCancel:
         dispatchCancel(msg);
         break;
      case OnBye:
         dispatchBye(msg);
         break;
      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void
ServerInviteSession::dispatchAccepted(const SipMessage& msg)
{
   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);
   InfoLog (<< "dispatchAccepted: " << msg.brief());
   
   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnInvite:
      case OnInviteReliable:
      case OnInviteOffer:
      case OnInviteReliableOffer:
      case OnUpdate:
      case OnUpdateOffer:
      {
         SharedPtr<SipMessage> response(new SipMessage);
         mDialog.makeResponse(*response, msg, 491);
         send(response);
         break;
      }

      case OnAck:
      case OnAckAnswer:  // .bwc. unsolicited body in ACK; it would probably make sense to just ignore.
         mCurrentRetransmit200 = 0; // stop the 200 retransmit timer
         transition(Connected);
         handler->onConnectedConfirmed(getSessionHandle(), msg);  
         break;
      
      case OnCancel:
      {
         // Cancel and 200 crossed
         SharedPtr<SipMessage> c200(new SipMessage);
         mDialog.makeResponse(*c200, msg, 200);
         send(c200);
         break;
      }

      case OnBye:
      {
         SharedPtr<SipMessage> b200(new SipMessage);
         mDialog.makeResponse(*b200, msg, 200);
         send(b200);

         transition(Terminated);
         handler->onTerminated(getSessionHandle(), InviteSessionHandler::RemoteBye, &msg);
         mDum.destroy(this);
         break;
      }
        
      case OnPrack:
         assert(0);  // TODO why is this handling here - remove it?
         /*
         if(!handlePrack(msg))
         {
            return; // prack does not correspond to an unacknowedged provisional
         }
         
         if(offerAnswer.get())  // New offer
         {
            // TODO - what should we do if the PRACK recieved after we accept the 
            // call has an offer in it.  Should we process it? or Ignore it?
            // Ignoring for now - should at least reject
            // 
            // TODO - reject PRACK, teardown call?
            ErrLog (<< "PRACK with new offer when in state=" << toData(mState));
            assert(false);
            return;

            if(!mAnswerSentReliably)    // new offer before previous answer sent 
            {
               // TODO - reject PRACK, teardown call?
               ErrLog (<< "PRACK with new offer when in state=" << toData(mState));
               return;
            }
            else
            {
               // dispatch offer here and respond with 200OK in provideAnswer
               transition(UAS_NegotiatedReliable);  // TODO - this doesn't look right - needs review
               mPrackWithOffer = resip::SharedPtr<SipMessage>(new SipMessage(msg));
               mProposedRemoteOfferAnswer = InviteSession::makeOfferAnswer(*offerAnswer);
               mCurrentEncryptionLevel = getEncryptionLevel(msg);
               if(!isTerminated())  
               {
                  handler->onOffer(getSessionHandle(), msg, *offerAnswer);
               }
            }
         }
         else
         {
             SharedPtr<SipMessage> p200(new SipMessage);
             mDialog.makeResponse(*p200, msg, 200);
             send(p200);
             prackCheckQueue();
         }*/
         break;
      
      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void
ServerInviteSession::dispatchWaitingToOffer(const SipMessage& msg)
{
   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);
   InfoLog (<< "dispatchWaitingToOffer: " << msg.brief());
   
   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnInvite:
      case OnInviteReliable:
      case OnInviteOffer:
      case OnInviteReliableOffer:
      case OnUpdate:
      case OnUpdateOffer:
      {
         SharedPtr<SipMessage> response(new SipMessage);
         mDialog.makeResponse(*response, msg, 491);
         send(response);
         break;
      }

      case OnAck:
      //case OnAckAnswer:   // illegal but accept anyway for improved interop - TODO - do we want this or the reject below?
         assert(mProposedLocalOfferAnswer.get());
         mCurrentRetransmit200 = 0; // stop the 200 retransmit timer
         provideProposedOffer(); 
         break;
      
      case OnAckAnswer:
         mCurrentRetransmit200 = 0; // stop the 200 retransmit timer
         sendBye();
         transition(Terminated);
         handler->onTerminated(getSessionHandle(), InviteSessionHandler::Error, &msg); 
         break;
      
      case OnCancel:
      {
         // Cancel and 200 crossed
         SharedPtr<SipMessage> c200(new SipMessage);
         mDialog.makeResponse(*c200, msg, 200);
         send(c200);
         break;
      }

      case OnBye:
      {
         SharedPtr<SipMessage> b200(new SipMessage);
         mDialog.makeResponse(*b200, msg, 200);
         send(b200);

         transition(Terminated);
         handler->onTerminated(getSessionHandle(), InviteSessionHandler::RemoteBye, &msg);
         mDum.destroy(this);
         break;
      }
              
      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void
ServerInviteSession::dispatchWaitingToRequestOffer(const SipMessage& msg)
{
   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);
   InfoLog (<< "dispatchWaitingToRequestOffer: " << msg.brief());
   
   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnInvite:
      case OnInviteReliable:
      case OnInviteOffer:
      case OnInviteReliableOffer:
      case OnUpdate:
      case OnUpdateOffer:
      {
         SharedPtr<SipMessage> response(new SipMessage);
         mDialog.makeResponse(*response, msg, 491);
         send(response);
         break;
      }

      case OnAck:
      //case OnAckAnswer:   // illegal but accept anyway for improved interop  - TODO do we want this or the reject below
         mCurrentRetransmit200 = 0; // stop the 200 retransmit timer
         requestOffer(); 
         break;

      case OnAckAnswer:
         mCurrentRetransmit200 = 0; // stop the 200 retransmit timer
         sendBye();
         transition(Terminated);
         handler->onTerminated(getSessionHandle(), InviteSessionHandler::Error, &msg); 
         break;
      
      case OnCancel:
      {
         // Cancel and 200 crossed
         SharedPtr<SipMessage> c200(new SipMessage);
         mDialog.makeResponse(*c200, msg, 200);
         send(c200);
         break;
      }

      case OnBye:
      {
         SharedPtr<SipMessage> b200(new SipMessage);
         mDialog.makeResponse(*b200, msg, 200);
         send(b200);

         transition(Terminated);
         handler->onTerminated(getSessionHandle(), InviteSessionHandler::RemoteBye, &msg);
         mDum.destroy(this);
         break;
      }
              
      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void
ServerInviteSession::dispatchAcceptedWaitingAnswer(const SipMessage& msg)
{
   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);

   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnInvite:
      case OnInviteReliable:
      case OnInviteOffer:
      case OnInviteReliableOffer:
      case OnUpdate:
      case OnUpdateOffer:
      {
         SharedPtr<SipMessage> response(new SipMessage);
         mDialog.makeResponse(*response, msg, 491);
         send(response);
         break;
      }

      case OnAckAnswer:
         mCurrentRetransmit200 = 0; // stop the 200 retransmit timer
         transition(Connected);
         setCurrentLocalOfferAnswer(msg);
         mCurrentEncryptionLevel = getEncryptionLevel(msg);
         mCurrentRemoteOfferAnswer = InviteSession::makeOfferAnswer(*offerAnswer);
         handler->onAnswer(getSessionHandle(), msg, *offerAnswer);
         if(!isTerminated())  // onAnswer callback may call end() or reject()
         {
            handler->onConnected(getSessionHandle(), msg);
         }
         break;

      case OnAck:
         mCurrentRetransmit200 = 0; // stop the 200 retransmit timer
         mEndReason = IllegalNegotiation;
         sendBye();
         transition(Terminated);
         handler->onTerminated(getSessionHandle(), InviteSessionHandler::Error, &msg);
         break;
         
      case OnCancel:
      {
         // no transition
         SharedPtr<SipMessage> c200(new SipMessage);
         mDialog.makeResponse(*c200, msg, 200);
         send(c200);
         break;
      }

      case OnPrack:
         assert(0);  // TODO why is this handling here - remove it?
         /*
         if(!handlePrack(msg))
         {
            return; // prack does not correspond to an unacknowedged provisional
         }

         if(offerAnswer.get())
         {
            // TODO - reject PRACK, teardown call?
            ErrLog (<< "PRACK with new offer when in state=" << toData(mState));
            return;
         }
         else
         {
            SharedPtr<SipMessage> p200(new SipMessage);
            mDialog.makeResponse(*p200, msg, 200);
            send(p200);
            //transition(UAS_NoAnswerReliable);  // TODO !slg!  this doesn't seem right - we've already Accepted the call
            //prackCheckQueue();
         }*/
         break;

      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void
ServerInviteSession::dispatchFirstSentOfferReliable(const SipMessage& msg)
{
   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);

   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnCancel:
         dispatchCancel(msg);
         break;

      case OnBye:
          dispatchBye(msg);
          break;

      case OnPrack:
         if(!handlePrack(msg))
         {
            return; // prack does not correspond to an unacknowedged provisional
         }
         
         if(offerAnswer.get())  // Answer
         {
            SharedPtr<SipMessage> p200(new SipMessage);
            mDialog.makeResponse(*p200, msg, 200);
            send(p200);

            setCurrentLocalOfferAnswer(msg);
            mCurrentRemoteOfferAnswer = InviteSession::makeOfferAnswer(*offerAnswer);
            mCurrentEncryptionLevel = getEncryptionLevel(msg);
            handler->onAnswer(getSessionHandle(), msg, *offerAnswer);

            if( mState != UAS_Accepted)    // queued 200 OK was sent in handlePrack/onPRACK and state was changed there
            {
               transition(UAS_NegotiatedReliable);
            }
         }
         else
         {
            mEndReason = IllegalNegotiation;
            transition(Terminated);
            handler->onTerminated(getSessionHandle(), InviteSessionHandler::Error, &msg);

            // 406 the Prack
            SharedPtr<SipMessage> p406(new SipMessage);
            mDialog.makeResponse(*p406, msg, 406);
            send(p406);

            // 406 the Invite
            SharedPtr<SipMessage> i406(new SipMessage);
            mDialog.makeResponse(*i406, mFirstRequest, 406);
            send(i406);

            mDum.destroy(this);
         }
         break;

      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

bool
ServerInviteSession::handlePrack(const SipMessage& msg)
{
   InfoLog (<< "handlePrack");

   InviteSessionHandler* handler = mDum.mInviteSessionHandler;

   if(mUnacknowledgedReliableProvisional.get() && 
      mUnacknowledgedReliableProvisional->header(h_RSeq).value() == msg.header(h_RAck).rSequence() &&
      mUnacknowledgedReliableProvisional->header(h_CSeq).sequence() == msg.header(h_RAck).cSequence() &&
      mUnacknowledgedReliableProvisional->header(h_CSeq).method() == msg.header(h_RAck).method())
   {
       mUnacknowledgedReliableProvisional.reset();  // Clear storage we have received our PRACK

       handler->onPrack(getHandle(),msg);
       InfoLog (<< "Found matching provisional for PRACK.");
       return true;
   }

   InfoLog (<< "spurious PRACK in state=" << toData(mState));
   SharedPtr<SipMessage> p481(new SipMessage);
   mDialog.makeResponse(*p481, msg, 481);
   send(p481);
   return false;
}

void
ServerInviteSession::prackCheckQueue()
{
   InfoLog (<< "prackCheckQueue: " << mQueuedResponses.size() );
   if(mQueuedResponses.size() > 0 && mQueuedResponses.front().first < 200)
   {
      InfoLog (<< "Sending queued provisional" );
      sendProvisional(mQueuedResponses.front().first, mQueuedResponses.front().second);
      mQueuedResponses.pop_front();
   }
   else if(mQueuedResponses.size() > 0 && mQueuedResponses.front().first < 300)
   {
      InfoLog (<< "Sending queued 200 OK" );
      InviteSessionHandler* handler = mDum.mInviteSessionHandler;
      transition(UAS_Accepted);
      Contents* sdp = 0;
      if( !mAnswerSentReliably && mCurrentLocalOfferAnswer.get())
      {
         sdp = mCurrentLocalOfferAnswer.get();
      }
      sendAccept(mQueuedResponses.front().first, sdp);
      handler->onConnected(getSessionHandle(), *mInvite200);
      mQueuedResponses.clear();  // shouldn't be any provisionals or 200's behind us in queue - just clear the entire queue
   }
}

void
ServerInviteSession::updateCheckQueue()
{
   // TODO  - should we be skipping over or ignoring any provisionals in the queue?
   InfoLog (<< "updateCheckQueue: " << mQueuedResponses.size() );
   if(mQueuedResponses.size() > 0  && 
      mQueuedResponses.front().first >= 200 &&
      mQueuedResponses.front().first < 300)
   {
      InfoLog (<< "Sending queued 200 OK" );
      InviteSessionHandler* handler = mDum.mInviteSessionHandler;
      transition(UAS_Accepted);
      sendAccept(mQueuedResponses.front().first, 0);
      handler->onConnected(getSessionHandle(), *mInvite200);
      mQueuedResponses.pop_front();
   }
}

void 
ServerInviteSession::dispatchFirstSentAnswerReliable(const SipMessage& msg)
{
   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);

   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnCancel:
         dispatchCancel(msg);
         break;

      case OnBye:
         dispatchBye(msg);
         break;

      case OnPrack:
         if(!handlePrack(msg))
         {
            return; // prack does not correspond to an unacknowedged provisional
         }
         
         if(offerAnswer.get())  // New offer
         {
            // dispatch offer here and respond with 200OK in provideAnswer
            transition(UAS_NegotiatedReliable);
            mPrackWithOffer = resip::SharedPtr<SipMessage>(new SipMessage(msg));
            mProposedRemoteOfferAnswer = InviteSession::makeOfferAnswer(*offerAnswer);
            mCurrentEncryptionLevel = getEncryptionLevel(msg);
            if(!isTerminated())  
            {
               handler->onOffer(getSessionHandle(), msg, *offerAnswer);
            }
         }
         else
         {
             SharedPtr<SipMessage> p200(new SipMessage);
             mDialog.makeResponse(*p200, msg, 200);
             send(p200);
             if( mState != UAS_Accepted)    // queued 200 OK was sent in handlePrack/onPrack and state was changed there
             {
                transition(UAS_NegotiatedReliable);
             }
             prackCheckQueue();
         }
         break;

      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void 
ServerInviteSession::dispatchNoAnswerReliableWaitingPrack(const SipMessage& msg)
{
//   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);

   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnCancel:
         dispatchCancel(msg);
         break;

      case OnBye:
          dispatchBye(msg);
          break;

      case OnPrack:
         if(!handlePrack(msg))
         {
            return; // prack does not correspond to an unacknowedged provisional
         }
         
         if(offerAnswer.get())
         {
            // 2nd offer, we haven't answered the first one
            // TODO - reject PRACK, teardown call?
            ErrLog (<< "PRACK with new offer when in state=" << toData(mState));
            return;
         }
         else
         {
            SharedPtr<SipMessage> p200(new SipMessage);
            mDialog.makeResponse(*p200, msg, 200);
            send(p200);
            transition(UAS_NoAnswerReliable);
            prackCheckQueue();
         }
         break;

      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void
ServerInviteSession::dispatchSentUpdate(const SipMessage& msg)
{
   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);

   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnCancel:
         dispatchCancel(msg);
         break;

      case OnBye:
          dispatchBye(msg);
          break;

      case On200Update:
         transition(UAS_NegotiatedReliable);
         if (offerAnswer.get())
         {
            setCurrentLocalOfferAnswer(msg);
            mCurrentRemoteOfferAnswer = InviteSession::makeOfferAnswer(*offerAnswer);
            mCurrentEncryptionLevel = getEncryptionLevel(msg);
            handler->onAnswer(getSessionHandle(), msg, *offerAnswer);
         }
         break;

     case OnUpdateRejected:
         transition(UAS_NegotiatedReliable);
         mProposedLocalOfferAnswer.reset();
         handler->onOfferRejected(getSessionHandle(), &msg);
         break;

      case On491Update:
         transition(UAS_SentUpdateGlare);
         start491Timer();
         break;

      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void
ServerInviteSession::dispatchSentUpdateAccepted(const SipMessage& msg)
{
   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);

   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnCancel:
         dispatchCancel(msg);
         break;

      case OnBye:
          dispatchBye(msg);
          break;

      case On200Update:
         transition(UAS_Accepted);
         if (offerAnswer.get())
         {
            setCurrentLocalOfferAnswer(msg);
            mCurrentRemoteOfferAnswer = InviteSession::makeOfferAnswer(*offerAnswer);
            mCurrentEncryptionLevel = getEncryptionLevel(msg);
            handler->onAnswer(getSessionHandle(), msg, *offerAnswer);
         }
         updateCheckQueue();
         break;

      case OnUpdateRejected:
         transition(UAS_Accepted);
         mProposedLocalOfferAnswer.reset();
         handler->onOfferRejected(getSessionHandle(), &msg);
         updateCheckQueue();
         break;

      case On491Update:
         // strange case - we send an offer then called accept and the offer/update got 491'd - treating the same as any offer error
         transition(UAS_Accepted);
         mProposedLocalOfferAnswer.reset();
         handler->onOfferRejected(getSessionHandle(), &msg);
         updateCheckQueue();
         break;

      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void
ServerInviteSession::dispatchReceivedUpdate(const SipMessage& msg)
{
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);

   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnCancel:
         dispatchCancel(msg);
         break;

      case OnBye:
          dispatchBye(msg);
          break;

      case OnUpdate:
      case OnUpdateOffer:
         // A UAS that receives an UPDATE before it has generated a final response to a previous UPDATE on the 
         // same dialog MUST return a 500 response
         {
            SharedPtr<SipMessage> u500(new SipMessage);
            mDialog.makeResponse(*u500, msg, 500);
            send(u500);
         }
         break;

      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void
ServerInviteSession::dispatchReceivedUpdateWaitingAnswer(const SipMessage& msg)
{
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);

   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnCancel:
         dispatchCancel(msg);
         break;

      case OnBye:
          dispatchBye(msg);
          break;

      case OnUpdate:
      case OnUpdateOffer:
         // A UAS that receives an UPDATE before it has generated a final response to a previous UPDATE on the 
         // same dialog MUST return a 500 response
         {
            SharedPtr<SipMessage> u500(new SipMessage);
            mDialog.makeResponse(*u500, msg, 500);
            send(u500);
         }
         break;

      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void
ServerInviteSession::dispatchNegotiatedReliable(const SipMessage& msg)
{
   InviteSessionHandler* handler = mDum.mInviteSessionHandler;
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);

   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnCancel:
         dispatchCancel(msg);
         break;

      case OnBye:
          dispatchBye(msg);
          break;

      case OnPrack:
         if(!handlePrack(msg))
         {
            return; // prack does not correspond to an unacknowedged provisional
         }
         
         if(offerAnswer.get())  // New offer
         {
            // dispatch offer here and respond with 200OK in provideAnswer
            mPrackWithOffer = resip::SharedPtr<SipMessage>(new SipMessage(msg));
            mProposedRemoteOfferAnswer = InviteSession::makeOfferAnswer(*offerAnswer);
            mCurrentEncryptionLevel = getEncryptionLevel(msg);
            if(!isTerminated())  
            {
               handler->onOffer(getSessionHandle(), msg, *offerAnswer);
            }
         }
         else
         {
             SharedPtr<SipMessage> p200(new SipMessage);
             mDialog.makeResponse(*p200, msg, 200);
             send(p200);
             prackCheckQueue();
         }
         break;

      case OnUpdateOffer:
         *mLastRemoteSessionModification = msg;
         transition(UAS_ReceivedUpdate);
         mProposedRemoteOfferAnswer = InviteSession::makeOfferAnswer(*offerAnswer);
         mCurrentEncryptionLevel = getEncryptionLevel(msg);
         if(!isTerminated())  
         {
            handler->onOffer(getSessionHandle(), msg, *offerAnswer);
         }
         break;
         
      case OnUpdate:
         {
            // UPDATE with no offer just respond with 200
            SharedPtr<SipMessage> u200(new SipMessage);
            mDialog.makeResponse(*u200, msg, 200);
            send(u200);
         }
         break;

      default:
         if(msg.isRequest())
         {
            dispatchUnknown(msg);
         }
         break;
   }
}

void
ServerInviteSession::dispatchWaitingToTerminate(const SipMessage& msg)
{
}

void
ServerInviteSession::dispatchWaitingToHangup(const SipMessage& msg)
{
   std::auto_ptr<Contents> offerAnswer = InviteSession::getOfferAnswer(msg);

   switch (toEvent(msg, offerAnswer.get()))
   {
      case OnAck:
      case OnAckAnswer:
      {
         mCurrentRetransmit200 = 0; // stop the 200 retransmit timer

         SharedPtr<SipMessage> msg = sendBye();
         transition(Terminated);
         mDum.mInviteSessionHandler->onTerminated(getSessionHandle(), InviteSessionHandler::LocalBye, msg.get());
         break;
      }
      
      default:
         break;
   }
}

void
ServerInviteSession::dispatchCancel(const SipMessage& msg)
{
   SharedPtr<SipMessage> c200(new SipMessage);
   mDialog.makeResponse(*c200, msg, 200);
   send(c200);

   SharedPtr<SipMessage> i487(new SipMessage);
   mDialog.makeResponse(*i487, mFirstRequest, 487);
   send(i487);

   transition(Terminated);

   if (mDum.mDialogEventStateManager)
   {
      mDum.mDialogEventStateManager->onTerminated(mDialog, msg, InviteSessionHandler::RemoteCancel);
   }

   mDum.mInviteSessionHandler->onTerminated(getSessionHandle(), InviteSessionHandler::RemoteCancel, &msg);
   mDum.destroy(this);
}

void
ServerInviteSession::dispatchBye(const SipMessage& msg)
{
   SharedPtr<SipMessage> b200(new SipMessage);
   mDialog.makeResponse(*b200, msg, 200);
   send(b200);
// !dcm! -- pretty sure we shouldn't 487 after the BYE/200
   SharedPtr<SipMessage> i487(new SipMessage);
   mDialog.makeResponse(*i487, mFirstRequest, 487);
   send(i487);

   transition(Terminated);

   mDum.mInviteSessionHandler->onTerminated(getSessionHandle(), InviteSessionHandler::RemoteBye, &msg);
   mDum.destroy(this);
}

void
ServerInviteSession::dispatchUnknown(const SipMessage& msg)
{
   InfoLog (<< "Unknown request (" << msg.brief() << ") received in state=" << toData(mState) << ", rejecting request and terminating call.");

   SharedPtr<SipMessage> r500(new SipMessage);
   mDialog.makeResponse(*r500, msg, 500);
   send(r500);
   
   SharedPtr<SipMessage> i400(new SipMessage);
   mDialog.makeResponse(*i400, mFirstRequest, 400);
   send(i400);

   transition(Terminated);
   mDum.mInviteSessionHandler->onTerminated(getSessionHandle(), InviteSessionHandler::Error, &msg);
   mDum.destroy(this);
}

void 
ServerInviteSession::startRetransmit1xxTimer()
{
   // RFC3261 13.3.1 says the UAS must send a non-100 provisional response every minute, to handle the possiblity of lost provisional responses
   int retransmissionTime = mDialog.mDialogSet.getUserProfile()->get1xxRetransmissionTime();
   if(retransmissionTime > 0 && m1xx->header(resip::h_StatusLine).statusCode() > 100)
   {
      mDum.addTimer(DumTimeout::Retransmit1xx, retransmissionTime, getBaseHandle(), ++mCurrentRetransmit1xxSeq);
   }
}

void
ServerInviteSession::startRetransmit1xxRelTimer()
{
   unsigned int seq = m1xx->header(h_RSeq).value();
   mDum.addTimerMs(DumTimeout::Retransmit1xxRel, Timer::T1, getBaseHandle(), seq, Timer::T1);
}

bool
ServerInviteSession::sendProvisional(int code, bool earlyFlag)
{
   m1xx->setContents(0);
   mDialog.makeResponse(*m1xx, mFirstRequest, code);

   bool sendReliably = code > 100 &&   // Must be a non-100 response
                       ((mFirstRequest.exists(h_Requires) && mFirstRequest.header(h_Requires).find(Token(Symbols::C100rel))) ||     // Far end requires it (note:  we must at least support it or the invite would have been rejected already)
                         mDum.getMasterProfile()->getUasReliableProvisionalMode() == MasterProfile::Required ||                     // OR we require it (note: far end must at least support it or the invite would have been rejected)
                         (mFirstRequest.exists(h_Supporteds) && mFirstRequest.header(h_Supporteds).find(Token(Symbols::C100rel)) && // OR far ends supports it and we support it 
                          mDum.getMasterProfile()->getUasReliableProvisionalMode() == MasterProfile::Supported));                   // (note:  SupportedEssential is not trapped here, cases for it are added below)

   switch (mState)
   {
      case UAS_OfferProvidedAnswer:
      case UAS_EarlyProvidedAnswer:
         if (code > 100 && earlyFlag && mCurrentLocalOfferAnswer.get()) // early media
         {
            setOfferAnswer(*m1xx, mCurrentLocalOfferAnswer.get());
         }
         break;

      case UAS_FirstSentAnswerReliable: // This is possible if someone called provisional() from the onPrack callback (since we change states after we call onPrack)
      case UAS_OfferReliableProvidedAnswer:  // MAY send answer in reliable response - send only if earlyFlag is set
         if (code > 100 && earlyFlag && !mAnswerSentReliably && mCurrentLocalOfferAnswer.get()) // early media
         {
            setOfferAnswer(*m1xx, mCurrentLocalOfferAnswer.get());
            mAnswerSentReliably = true;
            sendReliably = true;  // If UasReliableProvisionalMode is SupportEssential this may flip sendReliably to true
         }
         break;

      case UAS_NoOfferReliable:
         if(sendReliably)
         {
             // Invite requires reliable responses - since there was no offer in the INVITE we MUST 
             // provide an offer in the first reliable response.  We would be in the ProvidedOfferReliable
             // state if that was the case.  Looks like provisional was called too early!
             assert(false);
             return false;
         }
         break;

      case UAS_ProvidedOfferReliable:
         //  ignore early flag if we must sendReliably, since first reliable 1xx MUST contain Offer
         if (code > 100 && (earlyFlag || sendReliably) && mProposedLocalOfferAnswer.get())  
         {
            setOfferAnswer(*m1xx, mProposedLocalOfferAnswer.get());
            sendReliably = true; // If UasReliableProvisionalMode is SupportEssential this may flip sendReliably to true
         }
         break;

      case UAS_ProvidedOffer:
      case UAS_EarlyProvidedOffer:
         if (code > 100 && earlyFlag && mProposedLocalOfferAnswer.get()) 
         {
            setOfferAnswer(*m1xx, mProposedLocalOfferAnswer.get());
         }
         break;

      default:
         break;
   }
   DumHelper::setOutgoingEncryptionLevel(*m1xx, mProposedEncryptionLevel);

   if(sendReliably)
   {
      DebugLog ( << "Sending provisional reliably" );
      if (!m1xx->exists(h_Requires) ||
          !m1xx->header(h_Requires).find(Token(Symbols::C100rel)))
      {
         m1xx->header(h_Requires).push_back(Token(Symbols::C100rel));
      }
      m1xx->header(h_RSeq).value() = ++mLocalRSeq;

      assert(!mUnacknowledgedReliableProvisional.get());
      mUnacknowledgedReliableProvisional = m1xx;
      startRetransmit1xxRelTimer();
   }
   else
   {
      // Make sure there is no RSeq or Requires 100rel
      if(m1xx->exists(h_RSeq))
      {
         m1xx->remove(h_RSeq);
      }
      if(m1xx->exists(h_Requires))
      {
         ParserContainer<Token>::iterator it = m1xx->header(h_Requires).begin();
         for(; it != m1xx->header(h_Requires).end(); it++)
         {
            if((*it) == Token(Symbols::C100rel))
            {
               m1xx->header(h_Requires).erase(it);
               break;
            }
         }
      }
      startRetransmit1xxTimer();
   }

   if (mDum.mDialogEventStateManager)
   {
      mDum.mDialogEventStateManager->onEarly(mDialog, getSessionHandle());
   }

   send(m1xx);
   return sendReliably;
}

void
ServerInviteSession::queueResponse(int code, bool earlyFlag)
{
   InfoLog (<< "Response " << code << " queued." );
   mQueuedResponses.push_back( std::make_pair(code, earlyFlag) );
}

void
ServerInviteSession::sendAccept(int code, Contents* offerAnswer)
{
   mDialog.makeResponse(*mInvite200, mFirstRequest, code);
   handleSessionTimerRequest(*mInvite200, mFirstRequest);
   if (offerAnswer && !mAnswerSentReliably )
   {
      setOfferAnswer(*mInvite200, offerAnswer);
      mAnswerSentReliably = true;
   }
   mCurrentRetransmit1xxSeq++; // Stop the 1xx timer - causes timer to be ignored on expirey
   startRetransmit200Timer();  // 2xx timer
   DumHelper::setOutgoingEncryptionLevel(*mInvite200, mCurrentEncryptionLevel);

   if (mDum.mDialogEventStateManager)
   {
      mDum.mDialogEventStateManager->onConfirmed(mDialog, getSessionHandle());
   }

   send(mInvite200);
}

void
ServerInviteSession::sendUpdate(const Contents& offerAnswer)
{
   if (updateMethodSupported())
   {
      mDialog.makeRequest(*mLastLocalSessionModification, UPDATE);
      InviteSession::setOfferAnswer(*mLastLocalSessionModification, offerAnswer);
      DumHelper::setOutgoingEncryptionLevel(*mLastLocalSessionModification, mProposedEncryptionLevel);
      send(mLastLocalSessionModification);
   }
   else
   {
      throw UsageUseException("Can't send UPDATE to peer", __FILE__, __LINE__);
   }
}

/* ====================================================================
 * The Vovida Software License, Version 1.0 
 * 
 * Copyright (c) 2000 Vovida Networks, Inc.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the

 *    distribution.
 * 
 * 3. The names "VOCAL", "Vovida Open Communication Application Library",
 *    and "Vovida Open Communication Application Library (VOCAL)" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact vocal@vovida.org.
 *
 * 4. Products derived from this software may not be called "VOCAL", nor
 *    may "VOCAL" appear in their name, without prior written
 *    permission of Vovida Networks, Inc.
 * 
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL VOVIDA
 * NETWORKS, INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 * IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * 
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by Vovida
 * Networks, Inc. and many individuals on behalf of Vovida Networks,
 * Inc.  For more information on Vovida Networks, Inc., please see
 * <http://www.vovida.org/>.
 *
 */
