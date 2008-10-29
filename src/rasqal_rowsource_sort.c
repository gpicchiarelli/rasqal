/* -*- Mode: c; c-basic-offset: 2 -*-
 *
 * rasqal_rowsource_source.c - Rasqal source rowsource class
 *
 * Copyright (C) 2008, David Beckett http://www.dajobe.org/
 * 
 * This package is Free Software and part of Redland http://librdf.org/
 * 
 * It is licensed under the following three licenses as alternatives:
 *   1. GNU Lesser General Public License (LGPL) V2.1 or any newer version
 *   2. GNU General Public License (GPL) V2 or any newer version
 *   3. Apache License, V2.0 or any newer version
 * 
 * You may not use this file except in compliance with at least one of
 * the above three licenses.
 * 
 * See LICENSE.html or LICENSE.txt at the top of this package for the
 * complete terms and further detail along with the license texts for
 * the licenses in COPYING.LIB, COPYING and LICENSE-2.0.txt respectively.
 * 
 */


#ifdef HAVE_CONFIG_H
#include <rasqal_config.h>
#endif

#ifdef WIN32
#include <win32_rasqal_config.h>
#endif

#include <stdio.h>
#include <string.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include <raptor.h>

#include "rasqal.h"
#include "rasqal_internal.h"


#define DEBUG_FH stderr


typedef struct 
{
  rasqal_query *query;

  /* inner rowsource to sort */
  rasqal_rowsource *rowsource;

  /* map for sorting */
  rasqal_map* map;

  /* sequence of order conditions #rasqal_expression */
  raptor_sequence* seq;

  /* number of order conditions in query->order_conditions_sequence */
  int order_size;
} rasqal_sort_rowsource_context;


static int
rasqal_sort_rowsource_init(rasqal_rowsource* rowsource, void *user_data)
{
  rasqal_query *query;
  rasqal_sort_rowsource_context *con;

  con = (rasqal_sort_rowsource_context*)user_data;
  query = con->query;
  
  if(query->order_conditions_sequence)
    con->order_size = raptor_sequence_size(query->order_conditions_sequence);
  else {
    RASQAL_DEBUG1("No order conditions for sort rowsource - passing through");
    con->order_size = -1;
  }
  
  con->map = NULL;

  if(con->order_size > 0 ) {
    /* make a row:NULL map in order to sort or do distinct */
    con->map = rasqal_engine_new_rowsort_map(query->distinct,
                                             query->compare_flags,
                                             query->order_conditions_sequence);
    if(!con->map)
      return 1;
  }
  
  con->seq = NULL;

  return 0;
}


static int
rasqal_sort_rowsource_process(rasqal_rowsource* rowsource,
                              rasqal_sort_rowsource_context* con)
{
  int offset = 0;

  /* already processed */
  if(con->seq)
    return 0;
  
  con->seq = raptor_new_sequence((raptor_sequence_free_handler*)rasqal_free_row, 
                                 (raptor_sequence_print_handler*)rasqal_row_print);
  if(!con->seq)
    return 1;
  
  while(1) {
    rasqal_row* row;

    row = rasqal_rowsource_read_row(con->rowsource);
    if(!row)
      break;

    rasqal_row_set_order_size(row, con->order_size);

    rasqal_engine_rowsort_calculate_order_values(con->query, row);

    row->offset = offset;

    /* after this, row is owned by map */
    if(!rasqal_engine_rowsort_map_add_row(con->map, row))
      offset++;
  }
  
#ifdef RASQAL_DEBUG
  if(con->map) {
    fputs("resulting ", DEBUG_FH);
    rasqal_map_print(con->map, DEBUG_FH);
    fputs("\n", DEBUG_FH);
  }
#endif
  
  /* do sort/distinct: walk map in order, adding rows to sequence */
  rasqal_engine_rowsort_map_to_sequence(con->map, con->seq);
  rasqal_free_map(con->map); con->map = NULL;

  return 0;
}


static int
rasqal_sort_rowsource_ensure_variables(rasqal_rowsource* rowsource,
                                       void *user_data)
{
  rasqal_sort_rowsource_context* con;
  con = (rasqal_sort_rowsource_context*)user_data; 

  rasqal_rowsource_ensure_variables(con->rowsource);

  rowsource->size = 0;
  rasqal_rowsource_copy_variables(rowsource, con->rowsource);
  
  return 0;
}


static int
rasqal_sort_rowsource_finish(rasqal_rowsource* rowsource, void *user_data)
{
  rasqal_sort_rowsource_context *con;
  con = (rasqal_sort_rowsource_context*)user_data;

  if(con->rowsource)
    rasqal_free_rowsource(con->rowsource);
  
  if(con->map)
    rasqal_free_map(con->map);

  if(con->seq)
    raptor_free_sequence(con->seq);

  RASQAL_FREE(rasqal_sort_rowsource_context, con);

  return 0;
}


static raptor_sequence*
rasqal_sort_rowsource_read_all_rows(rasqal_rowsource* rowsource,
                                    void *user_data)
{
  rasqal_query *query;
  rasqal_sort_rowsource_context *con;
  raptor_sequence *seq = NULL;
  
  con = (rasqal_sort_rowsource_context*)user_data;
  query = con->query;

  /* if there were no ordering conditions, pass it all on to inner rowsource */
  if(con->order_size <= 0)
    return rasqal_rowsource_read_all_rows(con->rowsource);


  /* need to sort */
  if(rasqal_sort_rowsource_process(rowsource, con))
    return NULL;

  if(con->seq) {
    /* pass ownership of seq back to caller */
    seq = con->seq;
    con->seq = NULL;
  }
  
  return seq;
}


static rasqal_query*
rasqal_sort_rowsource_get_query(rasqal_rowsource* rowsource, void *user_data)
{
  rasqal_sort_rowsource_context *con;
  con = (rasqal_sort_rowsource_context*)user_data;
  return con->query;
}


static const rasqal_rowsource_handler rasqal_sort_rowsource_handler = {
  /* .version =          */ 1,
  "sort",
  /* .init =             */ rasqal_sort_rowsource_init,
  /* .finish =           */ rasqal_sort_rowsource_finish,
  /* .ensure_variables = */ rasqal_sort_rowsource_ensure_variables,
  /* .read_row =         */ NULL,
  /* .read_all_rows =    */ rasqal_sort_rowsource_read_all_rows,
  /* .get_query =        */ rasqal_sort_rowsource_get_query
};


rasqal_rowsource*
rasqal_new_sort_rowsource(rasqal_query *query,
                          rasqal_rowsource *rowsource,
                          raptor_sequence *seq)
{
  rasqal_sort_rowsource_context *con;
  int flags = 0;

  if(!query || !rowsource || !seq)
    return NULL;
  
  con = (rasqal_sort_rowsource_context*)RASQAL_CALLOC(rasqal_sort_rowsource_context, 1, sizeof(rasqal_sort_rowsource_context));
  if(!con)
    return NULL;

  con->query = query;
  con->rowsource = rowsource;
  con->seq = seq;

  return rasqal_new_rowsource_from_handler(con,
                                           &rasqal_sort_rowsource_handler,
                                           query->vars_table,
                                           flags);
}