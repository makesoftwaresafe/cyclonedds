// Copyright(c) 2006 to 2020 ZettaScale Technology and others
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License v. 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
// v. 1.0 which is available at
// http://www.eclipse.org/org/documents/edl-v10.php.
//
// SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause

#include <assert.h>

#include "dds/ddsrt/atomics.h"
#include "dds/ddsrt/log.h"
#include "dds__entity.h"
#include "dds__reader.h"
#include "dds__topic.h"
#include "dds__readcond.h"
#include "dds/ddsi/ddsi_serdata.h"

dds_entity_t dds_create_querycondition (dds_entity_t reader, uint32_t mask, dds_querycondition_filter_fn filter)
{
  dds_reader *rd;
  dds_readcond *cond;
  dds_return_t rc;

  if ((rc = dds_reader_lock (reader, &rd)) != DDS_RETCODE_OK)
    return rc;
  else if ((rc = dds_create_readcond_impl (&cond, rd, DDS_KIND_COND_QUERY, mask, filter)) != DDS_RETCODE_OK)
  {
    dds_reader_unlock (rd);
    return rc;
  }
  else
  {
    dds_entity_t const hdl = cond->m_entity.m_hdllink.hdl;
    dds_entity_init_complete (&cond->m_entity);
    dds_reader_unlock (rd);
    return hdl;
  }
}
