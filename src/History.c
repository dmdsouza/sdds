/*  =========================================================================
    History - Queue for DDS samples

    Copyright (c) the Contributors as noted in the AUTHORS file.

    This file is part of sDDS:
    http://wwwvs.cs.hs-rm.de.
    =========================================================================
 */

/*
   @header
    History - This class queues samples of one instance for either a DataReader
              or a DataWriter.
   @discuss
   @end
 */

#include "sDDS.h"

//  Local helper functions
rc_t
s_History_enqueue(History_t* self);

#ifdef SDDS_HAS_QOS_RELIABILITY
static rc_t
s_History_checkSeqNr(History_t* self, Topic_t* topic, Locator_t* loc, SDDS_SEQNR_BIGGEST_TYPE seqNr);
#endif

//  ---------------------------------------------------------------------------
//  Initializes this class

rc_t
sdds_History_init() {
    return SDDS_RT_OK;
}

//  ---------------------------------------------------------------------------
//  Setup an instance of this class

rc_t
sdds_History_setup(History_t* self, Sample_t* samples, unsigned int depth) {
    assert(self);
    assert(samples);
    self->samples = samples;
    self->depth = depth;
    self->in_needle = 0;
    self->out_needle = depth;
#ifdef SDDS_HAS_QOS_RELIABILITY
    for (int i=0; i<depth; i++){
        self->samples[i].seqNr = 0;
    }
#endif
    return SDDS_RT_OK;
}


static inline bool_t
s_History_full (History_t* self)
{
    if (self->in_needle == self->depth) {
        return true;
    }
    return false;
}


//  ---------------------------------------------------------------------------
//  Try to enqueue a sample into the history. If the history is full this call
//  will discard the oldest sample in case of RELIABILITY best effort and block
//  in case of RELIABILITY reliable until samples are taken out.

rc_t
sdds_History_enqueue_data(History_t* self, Data data) {
    assert(self);
    assert(data);

    if (s_History_full (self)) {
        return SDDS_RT_FAIL;
    }
    //  Insert sample into queue
    self->samples[self->in_needle].data = data;
    return s_History_enqueue(self);
}


//  ---------------------------------------------------------------------------
//  Try to enqueue a sample as buffer into the history. If the history is full
//  this call will discard the oldest sample in case of RELIABILITY best effort
//  and block in case of RELIABILITY reliable until samples are taken out. If
//  the buffer is going to be enqueued it will be decoded.

#ifdef SDDS_HAS_QOS_RELIABILITY
rc_t
sdds_History_enqueue_buffer(History_t* self, NetBuffRef_t* buff, SDDS_SEQNR_BIGGEST_TYPE seqNr) {
#else
rc_t
sdds_History_enqueue_buffer(History_t* self, NetBuffRef_t* buff) {
#endif
    assert(self);
    assert(buff);
#ifdef FEATURE_SDDS_TRACING_ENABLED
#ifdef FEATURE_SDDS_TRACING_HISTORY_ENQUEUE
    Trace_point(SDDS_TRACE_EVENT_HISTORY_ENQUEUE);
#endif
#endif

    if (s_History_full (self)) {
        return SDDS_RT_FAIL;
    }

    //  Insert sample into queue
    Topic_t* topic = buff->curTopic;
    Locator_t* loc = (Locator_t*) buff->locators->first_fn(buff->locators);
    Locator_upRef(loc);

#ifdef SDDS_HAS_QOS_RELIABILITY
    //  Check validity of sequence number
    if (topic->seqNrBitSize > 0){ // topic has seqNr
        if (s_History_checkSeqNr(self, topic, loc, seqNr) == SDDS_RT_OK){
            self->samples[self->in_needle].seqNr = seqNr;
        } else {
            SNPS_discardSubMsg(buff);
            Locator_downRef(loc);
            return SDDS_RT_FAIL;
        }

#ifdef UTILS_DEBUG
        if (topic->seqNrBitSize > 0){
            sdds_History_print(self);
        }
#endif
    }
#endif
    rc_t ret = SNPS_readData(buff, topic->Data_decode, (Data) self->samples[self->in_needle].data);
    if (ret == SDDS_RT_FAIL) {
        return ret;
    }
    self->samples[self->in_needle].instance = loc;
#ifdef FEATURE_SDDS_TRACING_ENABLED
    Trace_point(SDDS_TRACE_EVENT_STOP);
#endif
    return s_History_enqueue (self);
}


rc_t
s_History_enqueue(History_t* self) {
    //  Move the input needle to the next free slot. If the input needle is at
    //  the end of the array move it to the beginning.
    unsigned int in_needle_prev = self->in_needle;
    self->in_needle++;
    if (self->in_needle == self->depth) {
        self->in_needle = 0;
    }
    //  Move the input needle to depth to indicate that the queue is full.
    if (self->in_needle == self->out_needle) {
        self->in_needle = self->depth;
    }
    //  If the queue was previously empty set the output needle.
    if (self->out_needle == self->depth) {
        self->out_needle = in_needle_prev;
    }
    return SDDS_RT_OK;
}


//  ---------------------------------------------------------------------------
//  Takes the oldest sample out of the queue. Returns a pointer to the sample if
//  the history is not empty, otherwise NULL.

Sample_t*
sdds_History_dequeue(History_t* self) {
    assert(self);
    if (self->out_needle == self->depth) {
        return NULL;
    }
    //  Remove sample from queue
    Sample_t* sample = &self->samples[self->out_needle];
    Locator_downRef(sample->instance);
    //  Move the output needle to the next sample slot. If the output needle is
    //  at the end of the array move it to the beginning.
    unsigned int out_needle_prev = self->out_needle;
    self->out_needle++;
    if (self->out_needle == self->depth) {
        self->out_needle = 0;
    }
    //  Move the output needle to depth to indicate that the queue is empty.
    if (self->out_needle == self->in_needle) {
        self->out_needle = self->depth;
    }
    //  If the queue was previously full set the input needle.
    if (self->in_needle == self->depth) {
        self->in_needle = out_needle_prev;
    }
#ifdef FEATURE_SDDS_TRACING_ENABLED
#ifdef FEATURE_SDDS_TRACING_HISTORY_DEQUEUE
    Trace_point(SDDS_TRACE_EVENT_HISTORY_DEQUEUE);
#endif
#endif
    return sample;
}

#ifdef SDDS_HAS_QOS_RELIABILITY
static inline rc_t
s_History_checkSeqNr(History_t* self, Topic_t* topic, Locator_t* loc, SDDS_SEQNR_BIGGEST_TYPE seqNr) {
    rc_t ret = SDDS_RT_FAIL;

    SDDS_SEQNR_BIGGEST_TYPE highestSeqNrOfLoc = 0;
    for (int i=0; i<self->depth; i++){
        if (self->samples[i].instance == loc
        && self->samples[i].seqNr > highestSeqNrOfLoc){
            highestSeqNrOfLoc = self->samples[i].seqNr;
        }
    }

    switch(topic->seqNrBitSize){
        case (SDDS_QOS_RELIABILITY_SEQSIZE_BASIC):
            if ((highestSeqNrOfLoc == 0)
            ||  (seqNr > highestSeqNrOfLoc)
            ||  (highestSeqNrOfLoc == 15)) {
                ret = SDDS_RT_OK;
            }
           break;
        case (SDDS_QOS_RELIABILITY_SEQSIZE_SMALL):
            if ((highestSeqNrOfLoc == 0)
            ||  (seqNr > highestSeqNrOfLoc)
            ||  (highestSeqNrOfLoc == 255)) {
                ret = SDDS_RT_OK;
            }
           break;
        case (SDDS_QOS_RELIABILITY_SEQSIZE_BIG):
            if ((highestSeqNrOfLoc == 0)
            ||  (seqNr > highestSeqNrOfLoc)
            ||  (highestSeqNrOfLoc == 65536)) {
                ret = SDDS_RT_OK;
            }
           break;
        case (SDDS_QOS_RELIABILITY_SEQSIZE_HUGE):
            if ((highestSeqNrOfLoc == 0)
            ||  (seqNr > highestSeqNrOfLoc)
            ||  (highestSeqNrOfLoc == 4294967296)) {
                ret = SDDS_RT_OK;
            }
           break;
    }

    return ret;
}
#endif

#ifdef UTILS_DEBUG
void
sdds_History_print(History_t* self) {
    printf("History [id: %p] {\n", self);
    printf("    depth: %d,\n", self->depth);
    printf("    in needle: %d,\n", self->in_needle);
    printf("    out needle: %d,\n", self->out_needle);
#ifdef SDDS_HAS_QOS_RELIABILITY
    printf("    samples:\n");
    for (int i=0; i<self->in_needle; i++){
        printf("        instance: %d, seqNr: %d,\n", self->samples[i].instance, self->samples[i].seqNr);
    }
#endif
    printf("}\n");
}
#endif
