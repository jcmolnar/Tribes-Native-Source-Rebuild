#include "simBase.h"
#include "simEv.h"
#include "netEventManager.h"
#include "BitStream.h"
#include "netGhostManager.h"

#define DebugChecksum FOURCC('D','b','U','g')

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
extern "C" int g_netDiag;   // NETDIAG packet-trace arming counter (defined in netPacketStream.cpp)
// EVTCAP capture (strip before commit): when readPacket hits a guaranteed remote event whose class is
// not registered (Persistent::create == NULL — the objectives-mode reliable-stall trigger, pt.16), it
// records the WHOLE guaranteed-event train of that packet here as raw bits, so the unknown event's
// framing + payload can be decoded offline to recover its real wire tag / pack format. Stored in a
// global so it survives the console flood; read from JS AFTER pressing O via:
//   Module.ccall('wasmGetEvtCap','string',[],[])
// Always-on (an unregistered event never appears in working modes), latest-wins.
static char g_evtCapBuf[1100] = {0};
extern "C" EMSCRIPTEN_KEEPALIVE const char* wasmGetEvtCap() { return g_evtCapBuf; }
// EVTHIST (strip before commit): per-tag histogram of UNREGISTERED guaranteed events + one raw bit-sample
// per tag (from the event start). Each packet bails at its FIRST unknown tag, but across packets they lead
// with different tags, so this collects every unregistered tag. The TeamObjectiveEvent (objective text)
// stands out: highest count (many lore lines re-posted on entry) + payload = small objNum + Huffman string.
static int  g_evtTagCount[128] = {0};
static char g_evtTagRaw[128][2600];   // WASM-PORT: enlarged 560->2600 to capture the FULL event train past 1024 (find the trailing TeamObjectiveEvent lore)
static char g_evtHistBuf[1024] = {0};
extern "C" EMSCRIPTEN_KEEPALIVE const char* wasmGetEvtHist() {
   char* p = g_evtHistBuf; int cap = sizeof(g_evtHistBuf), w = 0;
   g_evtHistBuf[0] = 0;   // WASM-PORT: terminate up-front so an empty histogram returns "" not stale
   for(int i = 0; i < 128 && w < cap - 16; i++)
      if(g_evtTagCount[i]) w += snprintf(p + w, cap - w, "%d:%d ", 1024 + i, g_evtTagCount[i]);
   return g_evtHistBuf;
}
extern "C" EMSCRIPTEN_KEEPALIVE const char* wasmGetEvtTagRaw(int tag) {
   int i = tag - 1024;
   return (i >= 0 && i < 128) ? g_evtTagRaw[i] : "";
}
// SKIPSTUB (pt.26 binary search, strip before commit): live-tunable skip of ONE unregistered event
// tag, so the search can walk the unknown team-status event's payload width and reach the lore events
// behind it. g_skipBits == -1 disables (original bail behaviour). wasmSetSkip resets the hit counter so
// each trial reads clean; wasmClearEvtHist wipes the histogram so a NEW surfaced tag is unambiguous.
static int g_skipTag  = 1024;
static int g_skipBits = -1;
static int g_skipHits = 0;
extern "C" EMSCRIPTEN_KEEPALIVE void wasmSetSkip(int bits)   { g_skipBits = bits; g_skipHits = 0; }
extern "C" EMSCRIPTEN_KEEPALIVE void wasmSetSkipTag(int tag) { g_skipTag = tag; }
extern "C" EMSCRIPTEN_KEEPALIVE int  wasmGetSkipHits()       { return g_skipHits; }
extern "C" EMSCRIPTEN_KEEPALIVE void wasmClearEvtHist() {
   for(int i = 0; i < 128; i++) { g_evtTagCount[i] = 0; g_evtTagRaw[i][0] = 0; }
}
// SEQDIAG (pt.33, strip before commit): the ordered guaranteed-event stall. Records the live receive-seq
// state so we can see WHY buffered events (scores/chat/objectives) don't deliver. g_seqWaitCount = how many
// events are stuck in waitSeqEvents (a positive steady value = a permanent gap upstream). The skip fields
// capture the last skipped 1024's seq vs the expected nextRecvSeq + whether our seq-advance fired/drained.
static int g_seqNextRecv = -999, g_seqWaitCount = -1;
static int g_skipSeq = -999, g_skipExpected = -999, g_skipAdvanced = -1, g_skipDrained = -1;
// pt.34 gap-heal state: detect a frozen nextRecvSeq (permanent ordered-gap from a dropped packet) and recover.
static int g_healLastNextRecv = -999, g_healStuck = 0, g_healCount = 0;
extern "C" EMSCRIPTEN_KEEPALIVE int wasmGetHealCount() { return g_healCount; }
// pt.37 EVT-TAG CAPTURE (strip before commit): the objective text IS delivered but mis-decoded as a CHAT
// event (garbage in chat) => Kronos's TeamObjectiveEvent rides a DIFFERENT wire tag than our guessed table
// expects, colliding with a chat/say class. Arm wasmArmEvtTagCap(n) right before the server re-posts an
// objective (e.g. Team::setObjective), then read wasmGetEvtTagCap() = "tag/reg/class + raw bits" of the
// next n events so we learn the exact wire tag to remap TeamObjectiveEvent onto (+ decode the bits to
// confirm via the Huffman table).
static int  g_etcArm = 0, g_etcW = 0;
static char g_etcBuf[6000];
extern "C" EMSCRIPTEN_KEEPALIVE void wasmArmEvtTagCap(int n) { g_etcArm = n; g_etcW = 0; g_etcBuf[0] = 0; }
extern "C" EMSCRIPTEN_KEEPALIVE const char* wasmGetEvtTagCap() { return g_etcBuf; }
// pt.40 FULL-SECTION CAPTURE (strip before commit): the per-event tag-cap above starts mid-event (after the
// classTag), so a misaligned read can't be untangled. THIS captures the ENTIRE EventManager event section
// from its TRUE packet-aligned start (__evtSectionStart, the first event present-flag) so the framing can be
// walked event-by-event offline to find the FIRST event whose unpack width diverges (the desync source).
// wasmArmEvtSection() arms; the section bits are snapshotted at every packet's section-start into a temp, and
// COMMITTED to the output the moment that packet hits an unregistered tag (= the desynced/objective packet) or
// decodes a TeamObjectiveEvent -- i.e. we keep the aligned section of exactly the packet of interest.
static int  g_secArm = 0, g_secCommitted = 0;
static char g_secTmp[3200];
static char g_secBuf[3200];
extern "C" EMSCRIPTEN_KEEPALIVE void wasmArmEvtSection() { g_secArm = 1; g_secCommitted = 0; g_secBuf[0] = 0; }
extern "C" EMSCRIPTEN_KEEPALIVE const char* wasmGetEvtSection() { return g_secBuf; }
static char g_seqDiagBuf[256];
extern "C" EMSCRIPTEN_KEEPALIVE const char* wasmGetSeqDiag() {
   snprintf(g_seqDiagBuf, sizeof(g_seqDiagBuf),
      "nextRecv=%d waitQ=%d | lastSkip: seq=%d expected=%d advanced=%d drained=%d",
      g_seqNextRecv, g_seqWaitCount, g_skipSeq, g_skipExpected, g_skipAdvanced, g_skipDrained);
   return g_seqDiagBuf;
}
// OFFLINE STRING PROBE (pt.26, strip before commit): pack an ascii '0'/'1' bit-string into a real
// BitStream and run the engine's OWN Huffman readString at a given bit offset. Lets the captured
// objective-event train be decoded with the real Huffman tables (no respawn): a correct offset (a
// writeString start: compressed-flag + 8-bit len + tree-walk) yields readable lore text; a wrong
// offset yields junk / non-printable bytes. Returns "end=<bitpos> comp=<0|1> len=<n> text=<...>".
static char g_decBuf[320];
extern "C" EMSCRIPTEN_KEEPALIVE const char* wasmDecodeStringAt(const char* bits, int offset) {
   static BYTE packed[256];
   memset(packed, 0, sizeof(packed));
   int nb = 0;
   for(const char* p = bits; *p && nb < 256*8; p++) {
      if(*p != '0' && *p != '1') continue;
      if(*p == '1') packed[nb>>3] |= (1 << (nb & 7));   // LSB-first, matches BitStream::readFlag
      nb++;
   }
   BitStream bs(packed, (nb+7)>>3);
   bs.setCurPos(offset);
   int comp = bs.readFlag() ? 1 : 0;
   int len  = bs.readInt(8);
   bs.setCurPos(offset);            // rewind; readString re-reads the flag+len itself
   char tmp[256];
   bs.readString(tmp);
   snprintf(g_decBuf, sizeof(g_decBuf), "end=%d comp=%d len=%d text=%s", bs.getCurPos(), comp, len, tmp);
   return g_decBuf;
}
#endif

bool SimEvent::verifyNotServer(SimManager *mgr)
{
   if(mgr->getId() == 2048)
   {
      // uh-oh.
      Net::setLastError("Invalid packet.");
      return false;
   }
   return true;
}

namespace Net
{

void EventManager::onDeleteNotify(SimObject *object)
{
   if(object == polledSet)
      polledSet = NULL;
   Parent::onDeleteNotify(object);
}

void EventManager::onRemove()
{
   if(polledSet)
   {
      polledSet->deleteObject();
      polledSet = NULL;
   }
   Parent::onRemove();
}

EventManager::EventLink *EventManager::allocEventLink()
{
   if(freeList)
   {
      EventLink *ret = freeList;
      freeList = freeList->nextEvent;
      ret->evt = NULL;
      return ret;
   }
   else
   {
      EventLink *ret = new EventLink;
      ret->evt = NULL;
      return ret;
   }
}

void EventManager::freeEventLink(EventManager::EventLink *freed)
{
   if (freed->evt)
      delete freed->evt;
   freed->evt = NULL;
   freed->nextEvent = freeList;
   freeList = freed;
}

EventManager::EventManager()
{
   freeList = 0;
   sendQueueHead = NULL;
   sendQueueTail = NULL;
   waitSeqEvents = NULL;

   lastAckedSeq = 0;
   nextSendSeq = 0;
   nextRecvSeq = 0;
   highSentSeq = 0;
   ackMask[0] = 0;
   ackMask[1] = 0;
   ackMask[2] = 0;
   ackMask[3] = 0;


   polledSet = NULL;
}

EventManager::~EventManager()
{
   EventLink *temp;

   while(sendQueueHead)
   {
      temp = sendQueueHead->nextEvent;
      freeEventLink(sendQueueHead);
      sendQueueHead = temp;
   }
   while(waitSeqEvents)
   {
      temp = waitSeqEvents->nextEvent;
      freeEventLink(waitSeqEvents);
      waitSeqEvents = temp;
   }

   while(freeList)
   {
      temp = freeList->nextEvent;
      delete freeList;
      freeList = temp;
   }
}

void EventManager::addPolledObject(SimObject *obj)
{
   if(!polledSet)
   {
      polledSet = new SimSet(false);
      manager->addObject(polledSet);
      deleteNotify(polledSet);
   }
   polledSet->addObject(obj);
}

void EventManager::postRemoteEvent(const SimEvent *theEvent)
{
   if(owner->getStreamMode() == PacketStream::PlaybackMode)
   {
      delete theEvent;
      return;
   }

   EventLink *ev = allocEventLink();

   ev->evt = (SimEvent *) theEvent;
   if(theEvent->flags.test(SimEvent::Guaranteed))
   {
      ev->guaranteed = true;
      if(theEvent->flags.test(SimEvent::Ordered))
         ev->seqCount = nextSendSeq++;
      else
         ev->seqCount = -2;
   }
   else
   {
      ev->guaranteed = false;
   }
   ev->nextEvent = NULL;
   if(!sendQueueHead)
      sendQueueHead = sendQueueTail = ev;
   else
   {
      sendQueueTail->nextEvent = ev;
      sendQueueTail = ev;
   }
}

bool EventManager::writePacket(BitStream *bstream, DWORD &key)
{
   if(polledSet)
   {
      SimPolledUpdateEvent ev;
      ev.evManager = this;
      SimSet::iterator i;
      for(i = polledSet->begin(); i != polledSet->end(); i++)
         (*i)->processEvent(&ev);
   }

   if(!sendQueueHead)
      return false;

   EventLink *packQueueHead = NULL, *packQueueTail = NULL;
   int prevSeq = -4;

   while(sendQueueHead)
   {
		if(bstream->isFull())
			break;

      int idx = (lastAckedSeq >> 5) & 0x3;

      while(!(ackMask[idx] & (1 << (lastAckedSeq & 0x1F))) &&
         (lastAckedSeq < highSentSeq))
      {
         lastAckedSeq++;
         idx = (lastAckedSeq >> 5) & 0x3;
      }

      if(sendQueueHead->guaranteed && sendQueueHead->seqCount > lastAckedSeq + 126)
         break;

      EventLink *ev = sendQueueHead;
      sendQueueHead = sendQueueHead->nextEvent;

      // there is an event, so write a bit sayin so
      bstream->writeFlag(true);

      // write out guaranteeing / ordering data

      int startPos = bstream->getCurPos();

      if(ev->guaranteed)
      {
         bstream->writeFlag(true);
         ev->nextEvent = NULL;

         if(!packQueueHead)
            packQueueHead = packQueueTail = ev;
         else
         {
            packQueueTail->nextEvent = ev;
            packQueueTail = ev;
         }
         if(bstream->writeFlag(ev->seqCount == prevSeq + 1))
            prevSeq++;
         else
         {
            if(ev->seqCount == -2)
               bstream->writeFlag(false);
            else
            {
               bstream->writeFlag(true);
               bstream->writeInt(ev->seqCount,7);
            }
            prevSeq = ev->seqCount;
         }
         if(prevSeq != -2)
            ackMask[(prevSeq >> 5) & 0x3] |= 1 << (prevSeq & 0x1F);

         if(prevSeq > highSentSeq)
            highSentSeq = prevSeq;
      }
      else
         bstream->writeFlag(false);

      AssertFatal(ev->evt->type >= 1024 && ev->evt->type < 1152, "event type must be in 10-bit range for remote events");
      bstream->writeInt(ev->evt->type - 1024, 7);

      ev->evt->pack(manager, owner, bstream);

      PacketStream::getStats()->addBits(PacketStats::Send, bstream->getCurPos() - startPos, ev->evt->getPersistTag());
#ifdef DEBUG_NET
      // write out a checksum...
      // we'll check this on the other side for validation
      // later we can put in stuff to check sizes and such

      bstream->writeInt(ev->evt->type ^ DebugChecksum, 32);
#endif

      if(!ev->guaranteed)
         freeEventLink(ev);
   }

   bstream->writeFlag(false);

   if(packQueueHead)
      key = (DWORD) packQueueHead;
   else
      key = 0;

   return true;
}

bool EventManager::sendEvent(SimEvent *event)
{
   SimObject *obj = event->address.resolve(manager);
   if(obj && !obj->isDeleted())
      obj->processEvent(event);
   delete event;
   return getLastError()[0] == 0;
}

void EventManager::readPacket(BitStream *bstream, DWORD)
{
   int prevSeq = -4;
#ifdef __EMSCRIPTEN__
   // EVTCAP: first bit of this manager's event section, so the UNREGISTERED-event capture below can
   // dump the entire guaranteed-event train (every event read this packet) from its true start.
   int __evtSectionStart = bstream->getCurPos();
   // pt.40 full-section snapshot: grab this packet's whole section from its aligned start into a temp; it is
   // committed below only if this packet turns out to be the one of interest (unregistered tag / objective).
   if(g_secArm && !g_secCommitted) {
      int __save = bstream->getCurPos();
      int __nb = 0;
      for(; __nb < 3000 && bstream->isValid(); __nb++) g_secTmp[__nb] = bstream->readFlag() ? '1' : '0';
      g_secTmp[__nb] = 0;
      bstream->setCurPos(__save);
   }
#endif
   while(bstream->readFlag())
   {
      int startPos = bstream->getCurPos();

      bool guaranteed = bstream->readFlag();
      int seq = -2;

      if(guaranteed)
      {
         if(bstream->readFlag())
            seq = (prevSeq + 1) & 0x7F;
         else
         {
            if(bstream->readFlag())
               seq = bstream->readInt(7);
            else
               seq = -2;
         }
         prevSeq = seq;
      }
      int classTag = bstream->readInt(7) + 1024;
      SimEvent *evt = (SimEvent *) Persistent::create(classTag);
#ifdef __EMSCRIPTEN__
      // pt.40: this is the packet of interest if it carries an objective (1033) or hits a desync (unregistered
      // tag). Commit the aligned full-section snapshot taken at this packet's start so it can be walked offline.
      if(g_secArm && !g_secCommitted && (classTag == 1113 || !evt)) {   // 1113 = TeamObjectiveEvent (the burst we want)
         strcpy(g_secBuf, g_secTmp); g_secCommitted = 1;
      }
      // pt.37 EVT-TAG CAPTURE: record tag/registered?/class + raw payload bits (from just after classTag)
      // for the next g_etcArm events so we can identify the objective event's true wire tag + decode it.
      if(g_etcArm > 0 && g_etcW < (int)sizeof(g_etcBuf) - 160) {
         g_etcArm--;
         int save = bstream->getCurPos();           // = end of the 7-bit classTag = payload start
         char bits[420]; int nb = 0;
         for(; nb < 400 && bstream->isValid(); nb++) bits[nb] = bstream->readFlag() ? '1' : '0';
         bits[nb] = 0;
         bstream->setCurPos(save);
         g_etcW += snprintf(g_etcBuf + g_etcW, sizeof(g_etcBuf) - g_etcW,
            "[tag=%d reg=%d class=%s seq=%d payload=%s]\n",
            classTag, evt ? 1 : 0, evt ? evt->getClassName() : "?", seq, bits);
      }
#endif
      if(!evt)
      {
#ifdef __EMSCRIPTEN__
         // EVTHIST: count this unregistered tag and keep one raw bit-sample per tag (from the event start,
         // so framing guar(1)+seqEnc+classTag(7) then payload sits at offset 0). read-only (save/restore).
         {
            int idx = classTag - 1024;
            if(idx >= 0 && idx < 128) {
               g_evtTagCount[idx]++;
               if(g_evtTagRaw[idx][0] == 0) {
                  int save = bstream->getCurPos();
                  bstream->setCurPos(startPos);
                  int nb = 0;
                  for(; nb < 2560 && bstream->isValid(); nb++)
                     g_evtTagRaw[idx][nb] = bstream->readFlag() ? '1' : '0';
                  g_evtTagRaw[idx][nb] = 0;
                  bstream->setCurPos(save);
               }
            }
         }
         // SKIPSTUB (pt.33): consume exactly g_skipBits payload bits of the unregistered g_skipTag
         // (Kronos's binary team-status event 1024, width 227 = 23-bit header + 4 House records x 51),
         // then CONTINUE past it instead of bailing. The guar/seq/classTag framing was already read
         // above, so g_skipBits is just the post-classTag payload width; a wrong width misaligns PSC/ghost
         // -> disconnect, the right width keeps us connected. **pt.33 THE FIX:** 1024 is guaranteed+ORDERED,
         // so we must ALSO advance the receive sequence as if it were delivered -- otherwise its seq stays
         // an unfilled gap and EVERY later ordered event (scores/chat/TeamObjective lore) piles up in
         // waitSeqEvents and never delivers (the "text only loads after I press Tab / send chat" symptom:
         // the buffer only drains when an unrelated retransmit happens to fill the gap). Packet-level DNet
         // ack already stops 1024's retransmit; here we replicate the in-order delivery bookkeeping from
         // the normal path below (advance nextRecvSeq + drain the wait-queue) for this no-op event.
         if(classTag == g_skipTag && g_skipBits >= 0)
         {
            g_skipHits++;
            for(int b = 0; b < g_skipBits && bstream->isValid(); b++)
               bstream->readFlag();
            g_skipSeq = seq; g_skipExpected = nextRecvSeq; g_skipAdvanced = 0; g_skipDrained = 0;
            if(seq != -2 && seq == nextRecvSeq)
            {
               g_skipAdvanced = 1;
               nextRecvSeq = (nextRecvSeq + 1) & 0x7F;
               EventLink **walk = &waitSeqEvents;
               while(*walk)
               {
                  EventLink *ev = *walk;
                  if(ev->seqCount == nextRecvSeq)
                  {
                     nextRecvSeq = (nextRecvSeq + 1) & 0x7F;
                     g_skipDrained++;
                     *walk = ev->nextEvent;
                     SimEvent *drainEvt = ev->evt;
                     ev->evt = NULL;
                     walk = &waitSeqEvents;
                     freeEventLink(ev);
                     if(!sendEvent(drainEvt))
                        return;
                  }
                  else
                     walk = &(ev->nextEvent);
               }
            }
            continue;
         }
#endif
         // WASM-PORT: name the unregistered remote-event class tag (was a bare
         // "Invalid packet."). Tags 1024+N map to simEvDcl.h SimTypeRange+N; the
         // game-side (Kronos) events live above the engine's range and must be
         // ported/registered for the live event stream to decode.
         printf("netEventManager: UNREGISTERED remote-event classTag=%d (SimTypeRange+%d)\n",
                classTag, classTag - 1024);
         setLastError("Invalid packet.");
         return;
      }
      evt->type = classTag;
      evt->sourceManagerId = owner->getId();

      // event unpack sets the address field for addressing
      evt->unpack(manager, owner, bstream);
#ifdef __EMSCRIPTEN__
      // EVTDIAG (strip before commit): the objectives-view desync is upstream of PlayerPSC, and the
      // PacketStreamClient order is EventManager -> PlayerPSC -> GhostManager (FearCSDelegate::
      // addStreamClients), so an EVENT whose unpack reads a wrong bit-count is the only thing that can
      // shift the stream before PSC. Gated on g_netDiag (armed at the mode transition) to stay silent
      // during normal play. Log each event's class + bits consumed to find the mismatch.
      if(g_netDiag > 0) {
         printf("[EVT] class=%s tag=%d guar=%d seq=%d start=%d end=%d bits=%d valid=%d\n",
                evt->getClassName(), (int)evt->type, (int)guaranteed, seq, startPos,
                bstream->getCurPos(), bstream->getCurPos() - startPos, (int)bstream->isValid());
         // EVTBITS: dump raw bits from this event's classTag onward. If unpack consumed the wrong
         // payload size (port tag-numbering / version skew vs the live V0.8.5 server), the bits AFTER
         // where unpack stopped are unconsumed payload that then desyncs PlayerPSC. Save/restore pos so
         // this is read-only. classTagPos = end-of-framing; unpackEnd = where unpack() left the cursor.
         {
            int unpackEnd = bstream->getCurPos();
            int save = unpackEnd;
            bstream->setCurPos(startPos);
            char bits[96]; int n = 0;
            for(int b = 0; b < 90 && n < 95; b++) bits[n++] = bstream->readFlag() ? '1' : '0';
            bits[n] = 0;
            bstream->setCurPos(save);
            printf("[EVTBITS] start=%d unpackEnd=%d +90raw=%s\n", startPos, unpackEnd, bits);
         }
         fflush(stdout);
      }
#endif
      if(getLastError()[0])
         return;

      PacketStream::getStats()->addBits(PacketStats::Receive, bstream->getCurPos() - startPos, evt->getPersistTag());
#ifdef DEBUG_NET
      int checksum = bstream->readInt(32);
      AssertFatal( (checksum ^ DebugChecksum) == (unsigned int)classTag,
         avar("unpack did not match pack for event of class %s.",
            evt->getClassName()) );
#endif
      // special event cases...
      // default behavior for using the ghost manager to deliver
      // events
      evt->flags.set(SimEvent::Remote);

      if(evt->flags.test(SimEvent::ToGhost) || evt->flags.test(SimEvent::ToGhostParent))
      {
         SimObject *destObject;
         if(evt->flags.test(SimEvent::ToGhost))
            destObject = owner->getGhostManager()->resolveGhost(evt->address.objectId);
         else
            destObject = owner->getGhostManager()->resolveGhostParent(evt->address.objectId);

         // only deliver to objects that exist...
         // if the object isn't there, tag it with bogus id

         if(destObject)
            evt->address.set(destObject);
         else
            evt->address.set(-2, 0);
      }

      if(seq != -2)
      {
#ifdef __EMSCRIPTEN__
         // pt.40 resilience: an objective (tag 1024) that's AHEAD of the ordered window would pile into
         // waitSeqEvents behind a stuck nextRecvSeq (a dropped-packet gap upstream sticks it forever) and
         // never render. Force-deliver it now — objective text is idempotent and the handler dedups by text,
         // so out-of-order delivery is harmless. We do NOT advance nextRecvSeq (the gap is real for other
         // event types); this only rescues the objectives that would otherwise be stranded in the queue.
         if(classTag == 1024 && seq != nextRecvSeq)
         {
            if(!sendEvent(evt))
               return;
         }
         else
#endif
         if(seq != nextRecvSeq)
         {
            EventLink *ev = allocEventLink();
            ev->seqCount = seq;
            ev->evt = evt;
            ev->nextEvent = waitSeqEvents;
            waitSeqEvents = ev;
         }
         else
         {
            if(!sendEvent(evt))
               return;

            nextRecvSeq = (nextRecvSeq + 1) & 0x7F;
            EventLink **walk = &waitSeqEvents;
            while(*walk)
            {
               EventLink *ev = *walk;
               if(ev->seqCount == nextRecvSeq)
               {
                  nextRecvSeq = (nextRecvSeq + 1) & 0x7F;
                  *walk = ev->nextEvent;
                  SimEvent *evt = ev->evt;
                  ev->evt = NULL;
                  walk = &waitSeqEvents;
                  freeEventLink(ev);
                  if(!sendEvent(evt))
                     return;
               }
               else
                  walk = &(ev->nextEvent);
            }
         }
      }
      else
      {
         if(!sendEvent(evt))
            return;
      }
   }
#ifdef __EMSCRIPTEN__
   // EVTDIAG (strip before commit): final handoff position — the next PacketStreamClient (PlayerPSC)
   // must ENTER here. If [RPKT] ENTER pos != this, the desync is inside one of the [EVT] events above.
   {
      // SEQDIAG (pt.33): snapshot the ordered-delivery state every packet. waitQ>0 steady = a permanent
      // gap holding back scores/chat/objectives.
      g_seqNextRecv = nextRecvSeq;
      int wq = 0; for(EventLink *w = waitSeqEvents; w; w = w->nextEvent) wq++;
      g_seqWaitCount = wq;
      if(g_netDiag > 0) {
         printf("[EVT] EM-DONE endPos=%d valid=%d nextRecv=%d waitQ=%d\n",
                bstream->getCurPos(), (int)bstream->isValid(), nextRecvSeq, wq);
         fflush(stdout);
      }
   }
   // pt.34 GAP-HEAL: the survive-the-desync guard (netPacketStream) DROPS undecodable packets to stay
   // connected, but the stock reliable layer assumes packets are never dropped (it disconnects instead) and
   // relies on retransmit. A dropped packet's guaranteed events are lost AND packet-acked, so the server
   // never resends them -> a permanent gap in the ORDERED seq -> nextRecvSeq freezes and every later ordered
   // event (scores/chat/TeamObjective lore) piles into waitSeqEvents forever (the "text only shows after I
   // interact" symptom; churn happens to deliver a NEWER copy that re-fills enough to drain). Heal it: if the
   // queue is non-empty and nextRecvSeq hasn't advanced for several packets, give up on the missing seq and
   // jump nextRecvSeq to the nearest buffered event, then drain in order. Losing one status event is harmless
   // (re-sent on the next refresh); a permanent stall is not.
   if(waitSeqEvents)
   {
      if(nextRecvSeq == g_healLastNextRecv) g_healStuck++;
      else { g_healStuck = 0; g_healLastNextRecv = nextRecvSeq; }
      if(g_healStuck >= 4)
      {
         // find the buffered event with the smallest forward distance from nextRecvSeq (the next deliverable)
         int bestDist = 1000; EventLink *best = NULL;
         for(EventLink *w = waitSeqEvents; w; w = w->nextEvent)
         {
            int d = (w->seqCount - nextRecvSeq) & 0x7F;
            if(d < bestDist) { bestDist = d; best = w; }
         }
         if(best)
         {
            g_healCount++;
            nextRecvSeq = best->seqCount;          // skip the lost seq(s)
            EventLink **walk = &waitSeqEvents;     // drain everything now in order
            while(*walk)
            {
               EventLink *ev = *walk;
               if(ev->seqCount == nextRecvSeq)
               {
                  nextRecvSeq = (nextRecvSeq + 1) & 0x7F;
                  *walk = ev->nextEvent;
                  SimEvent *healEvt = ev->evt; ev->evt = NULL;
                  walk = &waitSeqEvents;
                  freeEventLink(ev);
                  sendEvent(healEvt);
               }
               else walk = &(ev->nextEvent);
            }
         }
         g_healStuck = 0; g_healLastNextRecv = nextRecvSeq;
      }
   }
   else { g_healStuck = 0; g_healLastNextRecv = nextRecvSeq; }
#endif
}

void EventManager::packetDropped(DWORD key)
{
   // put these on the front of the event queue to be sent

   EventLink *evtList = (EventLink *) key;
   EventLink *walk = evtList;
   while(walk->nextEvent)
      walk = walk->nextEvent;

   walk->nextEvent = sendQueueHead;
   if(!walk->nextEvent)
      sendQueueTail = walk;

   sendQueueHead = evtList;
}

void EventManager::packetReceived(DWORD key)
{
   EventLink *evtList = (EventLink *) key;
   while(evtList)
   {
      EventLink *temp = evtList->nextEvent;
      if(evtList->guaranteed && evtList->seqCount != -2)
      {
         int idx = (evtList->seqCount >> 5) & 0x3;
         int bitMask = ~(1 << (evtList->seqCount & 0x1F));
         ackMask[idx] &= bitMask;
      }
      freeEventLink(evtList);
      evtList = temp;
   }
}

};