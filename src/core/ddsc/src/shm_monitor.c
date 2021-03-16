/*
 * Copyright(c) 2021 Apex.AI Inc. All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */


#include "shm__monitor.h"
#include "dds__types.h"
#include "dds__entity.h"

#if defined (__cplusplus)
extern "C" {
#endif

void shm_monitor_init(shm_monitor_t* monitor) {
    ddsrt_mutex_init(&monitor->m_lock);
        
    monitor->m_waitset = iox_ws_init(&monitor->m_waitset_storage);
    monitor->m_wakeup_trigger = iox_user_trigger_init(&monitor->m_wakeup_trigger_storage);
    iox_ws_attach_user_trigger_event(monitor->m_waitset, monitor->m_wakeup_trigger, 0, NULL);

    monitor->m_number_of_modifications_pending = 0;
    monitor->m_number_of_attached_readers = 0;
    for(int32_t i=0; i<SHM_MAX_NUMBER_OF_READERS; ++i) {
        monitor->m_readers_to_attach[i] = NULL;
        monitor->m_readers_to_detach[i] = NULL;
    }

    monitor->m_run_state = SHM_monitor_RUN;
    ddsrt_threadattr_t attr;
    ddsrt_threadattr_init (&attr);
    dds_return_t rc = ddsrt_thread_create(&monitor->m_thread, "shm_monitor_thread", 
                                          &attr, shm_monitor_thread, monitor);
    if(rc != DDS_RETCODE_OK) {
        monitor->m_run_state = SHM_monitor_NOT_RUNNING;
    }
}

void shm_monitor_destroy(shm_monitor_t* monitor) {    
    if(monitor->m_run_state != SHM_monitor_NOT_RUNNING) {
        monitor->m_run_state = SHM_monitor_STOP;
        shm_monitor_wake(monitor);
        uint32_t result;
        dds_return_t rc = ddsrt_thread_join(monitor->m_thread, &result);

        if(rc == DDS_RETCODE_OK) {
            monitor->m_run_state = SHM_monitor_NOT_RUNNING;
        }
    }
    //note: we must ensure no readers are actively using the monitor anymore,
    //the monitor and thus the waitset is to be destroyed after all readers are destroyed
    iox_ws_deinit(monitor->m_waitset);

    ddsrt_mutex_destroy(&monitor->m_lock);
}

dds_return_t shm_monitor_wake(shm_monitor_t* monitor) {
    iox_user_trigger_trigger(monitor->m_wakeup_trigger);
    return DDS_RETCODE_OK;
}

dds_return_t shm_monitor_attach_reader(shm_monitor_t* monitor, struct dds_reader* reader) {
    //TODO: using the pointer could be the fastest way and could be safe without deferred attach/detach (?)
    //TODO: what to use to get the handle ? Or should we pass the handle? The entity? - What is idiomatic here?
    dds_entity_t handle = reader->m_entity.m_hdllink.hdl;
    uint64_t reader_id = (uint64_t) handle;
 
    if(iox_ws_attach_subscriber_event(monitor->m_waitset, reader->m_iox_sub, SubscriberEvent_HAS_DATA, reader_id, NULL) !=WaitSetResult_SUCCESS) {  
        printf("error attaching reader %p\n", reader);      
        return DDS_RETCODE_OUT_OF_RESOURCES;
    }
    ++monitor->m_number_of_attached_readers;

    printf("attached reader %p with id %ld\n", reader, reader_id);
    return DDS_RETCODE_OK;
}

dds_return_t shm_monitor_detach_reader(shm_monitor_t* monitor, struct dds_reader* reader) {
    iox_ws_detach_subscriber_event(monitor->m_waitset, reader->m_iox_sub, SubscriberEvent_HAS_DATA);   
    --monitor->m_number_of_attached_readers;

    printf("detached reader %p\n", reader);
    return DDS_RETCODE_OK;
}

dds_return_t shm_monitor_deferred_attach_reader(shm_monitor_t* monitor, struct dds_reader* reader) {
    ddsrt_mutex_lock(&monitor->m_lock);

    //store the attach request
    for(int32_t i=0; i<SHM_MAX_NUMBER_OF_READERS; ++i) {
        if(monitor->m_readers_to_attach[i] == NULL) {
            monitor->m_readers_to_attach[i] = reader;
            ++monitor->m_number_of_modifications_pending;
            shm_monitor_wake(monitor);

            ddsrt_mutex_unlock(&monitor->m_lock);
            return DDS_RETCODE_OK;
        }
    }

    ddsrt_mutex_unlock(&monitor->m_lock);   
    return DDS_RETCODE_OUT_OF_RESOURCES;
}

dds_return_t shm_monitor_deferred_detach_reader(shm_monitor_t* monitor, struct dds_reader* reader) {
    ddsrt_mutex_lock(&monitor->m_lock);

    for(int32_t i=0; i<SHM_MAX_NUMBER_OF_READERS; ++i) {
        if(monitor->m_readers_to_attach[i] == reader) {
            monitor->m_readers_to_attach[i] = NULL;

            ddsrt_mutex_unlock(&monitor->m_lock);           
            return DDS_RETCODE_OK; //not attached yet, but we do not need to attach and then detach it
        }
    }
    // it must have been already attached, store the detach request
    for(int32_t i=0; i<SHM_MAX_NUMBER_OF_READERS; ++i) {
        if(monitor->m_readers_to_detach[i] == NULL) {
            monitor->m_readers_to_detach[i] = reader;
            ++monitor->m_number_of_modifications_pending;
            shm_monitor_wake(monitor);

            ddsrt_mutex_unlock(&monitor->m_lock);
            return DDS_RETCODE_OK;
        }
    }

    ddsrt_mutex_unlock(&monitor->m_lock);    
    return DDS_RETCODE_OUT_OF_RESOURCES;
}

dds_return_t shm_monitor_perform_deferred_modifications(shm_monitor_t* monitor) {
    ddsrt_mutex_lock(&monitor->m_lock);
    //problem: we have potential races: some of these readers may not even exist anymore
    //         but due to limitations of the waitset we also cannot attach/detach them while waiting
    //         (which will happen often)
    
    if(monitor->m_number_of_modifications_pending == 0) {
        ddsrt_mutex_unlock(&monitor->m_lock);
        return DDS_RETCODE_OK;
    }
    
    dds_return_t rc = DDS_RETCODE_OK;
    for(int32_t i=0; i<SHM_MAX_NUMBER_OF_READERS; ++i) {
        if(monitor->m_readers_to_attach[i] != NULL) {
            if(shm_monitor_attach_reader(monitor, monitor->m_readers_to_attach[i]) != DDS_RETCODE_OK) {
                rc = DDS_RETCODE_OUT_OF_RESOURCES;
            }
            monitor->m_readers_to_attach[i] = NULL;
        }
        //note we cannot have the same pointer in attach AND detach requests (ensured by construction)
        if(monitor->m_readers_to_detach[i] != NULL) {
            shm_monitor_detach_reader(monitor, monitor->m_readers_to_detach[i]);
            monitor->m_readers_to_detach[i] = NULL;
        }
    }
    monitor->m_number_of_modifications_pending = 0;

    ddsrt_mutex_unlock(&monitor->m_lock);
    return rc;
}


uint32_t shm_monitor_thread(void* arg) {
    shm_monitor_t* monitor = arg; 
    uint64_t number_of_missed_events = 0;
    uint64_t number_of_events = 0;
    iox_event_info_t events[SHM_MAX_NUMBER_OF_READERS];

    while(monitor->m_run_state == SHM_monitor_RUN) {

        number_of_events = iox_ws_wait(monitor->m_waitset, events, SHM_MAX_NUMBER_OF_READERS,
                                       &number_of_missed_events);

        //should not happen as the waitset is designed is configured here
        assert(number_of_missed_events == 0);

        //we woke up either due to termination request, modification request or
        //because some reader got data
        //check all the events and handle them accordingly

        for (uint64_t i = 0; i < number_of_events; ++i) {
            iox_event_info_t event = events[i];
            if (iox_event_info_does_originate_from_user_trigger(event, monitor->m_wakeup_trigger))
            {
                //note: having an additional trigger for termination only
                //will cause issues with the reader_ids beyond our control 
                //what would its id be (and not collide with a reader id/handle)
                if(monitor->m_run_state != SHM_monitor_RUN) {
                    break;
                }
                //TODO: I sense a subtle race here in the waitset usage (by design)
                //      which may cause aus to miss wake ups
                iox_user_trigger_reset_trigger(monitor->m_wakeup_trigger);
                shm_monitor_perform_deferred_modifications(monitor);
                printf("shm monitor woke up\n");
            } else {
                //some reader got data, identify the reader
                uint64_t reader_id = iox_event_info_get_event_id(event);                             
                dds_entity_t handle = (dds_entity_t) reader_id;
                dds_entity* entity;             

                //TODO: this is potentially costly, we may be able to use the pointer directly
                //      when it is used differently (monitor vs. waitset, concurrent execution of
                //      chunk receive handling)
                dds_entity_pin(handle, &entity);

                if(!entity) {
                    printf("pinning reader %ld failed\n", reader_id);
                    continue; //pinning failed (?)
                }

                dds_reader* reader = (dds_reader*) entity;

                printf("reader %ld received data\n", reader_id);
                const void* chunk;
                while(iox_sub_take_chunk(reader->m_iox_sub, &chunk) == ChunkReceiveResult_SUCCESS) {                   
                    //TODO: call the receive handler
                    //read_callback(chunk, reader);
                }

                dds_entity_unpin(entity);
            }
        }
        //now that we woke up and performed the required actions
        //we will check for termination request and if there is none wait again
    }

    printf("shm monitor thread stopped\n");
    return 0;
}

void shm_wakeup_trigger_callback(iox_user_trigger_t trigger) {    
    printf("trigger callback %p\n", trigger);
    // we can perform custom actions here (with limitations)
}

void shm_subscriber_callback(iox_sub_t sub) {
    //we need to take the data here but also guarantee in a way that the reader is pinned
    //however, we have no access to the reader by only knowing the subscriber
    printf("subscriber callback %p\n", sub);
}


#if defined (__cplusplus)
}
#endif