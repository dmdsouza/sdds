/*
 * =====================================================================================
 *
 *       Filename:  DataSource.c
 *
 *    Description:  Impl for the datasource class
 *
 *        Version:  1.0
 *        Created:  22.02.2010 18:09:11
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  Kai Beckmann (kai), kai-oliver.beckmann@hs-rm.de
 *        Company:  Hochschule RheinMain - DOPSY Labor für verteilte Systeme
 *
 * =====================================================================================
 */

#include "sDDS.h"

#ifndef SDDS_PLATFORM_autobest
#include <stdlib.h>
#endif
#include <string.h>

struct _InstantSender_t {
    NetBuffRef_t highPrio;
    NetBuffRef_t out[SDDS_NET_MAX_OUT_QUEUE];
};
typedef struct _InstantSender_t InstantSender_t;

struct _DataSource_t {
#if SDDS_MAX_DATA_WRITERS > 0
    DataWriter_t writers[SDDS_MAX_DATA_WRITERS];
#endif
    InstantSender_t sender;
    unsigned int remaining_datawriter : 4;
};

static DataSource_t dsStruct;

static DataSource_t* self = &dsStruct;

void print_Pointer() {
    for (int i=0; i < SDDS_NET_MAX_OUT_QUEUE; i++) {
        Log_debug("%d p: (%d) %p\n", __LINE__, i, self->sender.out[i].addr->List_first);
    }
}

rc_t
DataSource_init(void) {
    self->remaining_datawriter = SDDS_MAX_DATA_WRITERS;

    //  Init instant sender
    //  The init method alloc a frame buffer and adds it to the structure
    NetBuffRef_init(&(self->sender.highPrio));
    NetBuffRef_init(&(self->sender.out[0]));
    NetBuffRef_init(&(self->sender.out[1]));
    return DataWriter_init();
}

#ifdef FEATURE_SDDS_DISCOVERY_ENABLED
rc_t
DataSource_getTopic(DDS_DCPSSubscription* st, topicid_t id, Topic_t** topic) {
    int i;
    for (i = 0; i < (SDDS_MAX_DATA_WRITERS - self->remaining_datawriter);
         i++) {
        if ((self->writers[i].topic->id == id)) {
            if (st != NULL) {
                st->key = self->writers[i].id;
                st->participant_key = BuiltinTopic_participantID;
                st->topic_id = self->writers[i].topic->id;
            }
            if (topic != NULL) {
                *topic = self->writers[i].topic;
            }
            return SDDS_RT_OK;
        }
    }

    return SDDS_RT_FAIL;
}
#endif

#ifdef FEATURE_SDDS_DISCOVERY_ENABLED
rc_t
DataSource_getDataWrites(DDS_DCPSPublication* pt, int* len) {
    int i = 0;
    *len = 0;

    for (i = 0; i < (SDDS_MAX_DATA_WRITERS - self->remaining_datawriter); i++) {
#ifdef FEATURE_SDDS_BUILTIN_TOPICS_ENABLED
        if (!BuildinTopic_isBuiltinTopic(self->writers[i].topic->id,
                                         self->writers[i].topic->domain)) {
#endif
        pt[*len].key = self->writers[i].id;
        pt[*len].participant_key = BuiltinTopic_participantID;
        pt[*len].topic_id = self->writers[i].topic->id;

        (*len)++;
#ifdef FEATURE_SDDS_BUILTIN_TOPICS_ENABLED
    }
#endif
    }
    return SDDS_RT_OK;
}
#endif

#if SDDS_MAX_DATA_WRITERS > 0
DataWriter_t*
DataSource_create_datawriter(Topic_t* topic, Qos qos,
                             Listener list, StatusMask mask) {
    (void) qos;
    (void) list;
    (void) mask;

    DataWriter_t* dw = NULL;

    if (self->remaining_datawriter == 0) {
        return NULL;
    }
    dw = &(self->writers[SDDS_MAX_DATA_WRITERS - self->remaining_datawriter]);

    dw->topic = topic;
    dw->id = (SDDS_MAX_DATA_WRITERS - self->remaining_datawriter);
    self->remaining_datawriter--;

#ifdef SDDS_QOS_LATENCYBUDGET
    dw->qos.latBudDuration = SDDS_QOS_DW_LATBUD - SDDS_QOS_LATBUD_COMM - SDDS_QOS_LATBUD_READ;
#endif
    return dw;
}
#endif // SDDS_MAX_DATA_WRITERS

NetBuffRef_t*
findFreeFrame(List_t* dest) {
    NetBuffRef_t* buffRef = NULL;

    bool_t sameAddr = false;
    for (int i = 0; i < SDDS_NET_MAX_OUT_QUEUE; i++) {
        List_t* try = self->sender.out[i].addr;
        if (dest != NULL && try != NULL) {
            Locator_t* loc = try->List_first(try);
            while (loc != NULL) {
                if (Locator_contains(dest, loc) == SDDS_RT_OK) {
                    buffRef = &(self->sender.out[i]);
                    sameAddr = true;
                    break;
                }
                loc = try->List_next(try);
            }
            if (sameAddr) {
                break;
            }
        }
    }
    if (buffRef == NULL) {
        for (int i = 0; i < SDDS_NET_MAX_OUT_QUEUE; i++) {
            if (self->sender.out[i].curPos == 0) {
                buffRef = &(self->sender.out[i]);
                break;
            }
        }
    }
    if (buffRef == NULL) {
        buffRef = &(self->sender.highPrio);
    }
    if (buffRef->curPos == 0) {
        SNPS_initFrame(buffRef);
    }
    if (sameAddr == false) {
        // write addr

        // here add the ref to the buff, addr is used when frame is update addr
        // in bufref
        Locator_t* loc = (Locator_t*) dest->List_first(dest);
        while (loc != NULL) {
            if (Locator_contains(buffRef->addr, loc) != SDDS_RT_OK) {
                buffRef->addr->List_add(buffRef->addr, loc);
                Locator_upRef(loc);
            }
            loc = (Locator_t*) dest->List_next(dest);
        }
    }
    return buffRef;
}

