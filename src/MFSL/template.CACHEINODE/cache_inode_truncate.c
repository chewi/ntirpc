/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * Ce logiciel est un serveur implementant le protocole NFS.
 *
 * Ce logiciel est r�gi par la licence CeCILL soumise au droit fran�ais et
 * respectant les principes de diffusion des logiciels libres. Vous pouvez
 * utiliser, modifier et/ou redistribuer ce programme sous les conditions
 * de la licence CeCILL telle que diffus�e par le CEA, le CNRS et l'INRIA
 * sur le site "http://www.cecill.info".
 *
 * En contrepartie de l'accessibilit� au code source et des droits de copie,
 * de modification et de redistribution accord�s par cette licence, il n'est
 * offert aux utilisateurs qu'une garantie limit�e.  Pour les m�mes raisons,
 * seule une responsabilit� restreinte p�se sur l'auteur du programme,  le
 * titulaire des droits patrimoniaux et les conc�dants successifs.
 *
 * A cet �gard  l'attention de l'utilisateur est attir�e sur les risques
 * associ�s au chargement,  � l'utilisation,  � la modification et/ou au
 * d�veloppement et � la reproduction du logiciel par l'utilisateur �tant
 * donn� sa sp�cificit� de logiciel libre, qui peut le rendre complexe �
 * manipuler et qui le r�serve donc � des d�veloppeurs et des professionnels
 * avertis poss�dant  des  connaissances  informatiques approfondies.  Les
 * utilisateurs sont donc invit�s � charger  et  tester  l'ad�quation  du
 * logiciel � leurs besoins dans des conditions permettant d'assurer la
 * s�curit� de leurs syst�mes et ou de leurs donn�es et, plus g�n�ralement,
 * � l'utiliser et l'exploiter dans les m�mes conditions de s�curit�.
 *
 * Le fait que vous puissiez acc�der � cet en-t�te signifie que vous avez
 * pris connaissance de la licence CeCILL, et que vous en avez accept� les
 * termes.
 *
 * ---------------------
 *
 * Copyright CEA/DAM/DIF (2005)
 *  Contributor: Philippe DENIEL  philippe.deniel@cea.fr
 *               Thomas LEIBOVICI thomas.leibovici@cea.fr
 *
 *
 * This software is a server that implements the NFS protocol.
 * 
 *
 * This software is governed by the CeCILL  license under French law and
 * abiding by the rules of distribution of free software.  You can  use,
 * modify and/ or redistribute the software under the terms of the CeCILL
 * license as circulated by CEA, CNRS and INRIA at the following URL
 * "http://www.cecill.info".
 *
 * As a counterpart to the access to the source code and  rights to copy,
 * modify and redistribute granted by the license, users are provided only
 * with a limited warranty  and the software's author,  the holder of the
 * economic rights,  and the successive licensors  have only  limited
 * liability.
 *
 * In this respect, the user's attention is drawn to the risks associated
 * with loading,  using,  modifying and/or developing or reproducing the
 * software by the user in light of its specific status of free software,
 * that may mean  that it is complicated to manipulate,  and  that  also
 therefore means  that it is reserved for developers  and  experienced
 * professionals having in-depth computer knowledge. Users are therefore
 * encouraged to load and test the software's suitability as regards their
 * requirements in conditions enabling the security of their systems and/or
 * data to be ensured and,  more generally, to use and operate it in the
 * same conditions as regards security.
 *
 * The fact that you are presently reading this means that you have had
 * knowledge of the CeCILL license and that you accept its terms.
 * ---------------------------------------
 */

/**
 * \file    cache_inode_truncate.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:28 $
 * \version $Revision: 1.19 $
 * \brief   Truncates a regular file.
 *
 * cache_inode_truncate.c : Truncates a regular file.
 *
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif


#include "fsal.h"
#include "LRU_List.h"
#include "log_functions.h"
#include "HashData.h"
#include "HashTable.h"
#include "cache_inode.h"
#include "cache_content.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <time.h>
#include <pthread.h>


/**
 *
 * cache_inode_truncate_sw: truncates a regular file specified by its cache entry.
 * 
 * Truncates a regular file specified by its cache entry.
 *
 * @param pentry    [INOUT] entry pointer for the fs object to be truncated. 
 * @param length    [IN]    wanted length for the file. 
 * @param pattr     [OUT]   attrtibutes for the file after the operation. 
 * @param ht        [INOUT] hash table used for the cache. Unused in this call (kept for protototype's homogeneity). 
 * @param pclient   [INOUT] ressource allocated by the client for the nfs management.
 * @param pcontext     [IN]    FSAL credentials 
 * @param pstatus   [OUT]   returned status.
 * @param use_mutex [IN]    if TRUE, mutex management is done, not if equal to FALSE.
 * 
 * @return CACHE_INODE_SUCCESS if operation is a success \n
 * @return CACHE_INODE_LRU_ERROR if allocation error occured when validating the entry
 *
 */
cache_inode_status_t cache_inode_truncate_sw( cache_entry_t        * pentry, 
                                              fsal_size_t            length,  
                                              fsal_attrib_list_t   * pattr, 
                                              hash_table_t         * ht, 
                                              cache_inode_client_t * pclient, 
                                              fsal_op_context_t          * pcontext, 
                                              cache_inode_status_t * pstatus, 
                                              int                    use_mutex )
{
  fsal_status_t          fsal_status ;
  cache_content_status_t cache_content_status ;
  
  /* Set the return default to CACHE_INODE_SUCCESS */
  *pstatus = CACHE_INODE_SUCCESS ;
  
  /* stats */
  pclient->stat.nb_call_total += 1 ;
  pclient->stat.func_stats.nb_call[CACHE_INODE_TRUNCATE] += 1 ;

  if( use_mutex )
    P( pentry->lock ) ;

  /* Only regular files can be truncated */
  if( pentry->internal_md.type != REGULAR_FILE )
    {
      *pstatus = CACHE_INODE_BAD_TYPE ;
      if( use_mutex )
        V( pentry->lock ) ;

      /* stats */
      pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_TRUNCATE] += 1 ;
      
      return *pstatus ;
    }

  /* Calls file content cache to operate on the cache */
  if( pentry->object.file.pentry_content != NULL )
    {
      if( cache_content_truncate( pentry->object.file.pentry_content, 
                                  length, 
                                  (cache_content_client_t *)pclient->pcontent_client, 
                                  &cache_content_status ) != CACHE_CONTENT_SUCCESS )
        {
          *pstatus = cache_content_error_convert( cache_content_status ) ;
          if( use_mutex )
            V( pentry->lock ) ;

          /* stats */
          pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_TRUNCATE] += 1 ;
                                  
          return *pstatus ;
        }

      /* Cache truncate succeeded, we must now update the size in the attributes */
      if( ( pentry->object.file.attributes.asked_attributes & FSAL_ATTR_SIZE ) ||
          ( pentry->object.file.attributes.asked_attributes & FSAL_ATTR_SPACEUSED ) )
        {
          pentry->object.file.attributes.filesize  = length ;
          pentry->object.file.attributes.spaceused = length ;
        }
      
      
      /* Set the time stamp values too */
      pentry->object.file.attributes.mtime.seconds  = time( NULL ) ;
      pentry->object.file.attributes.mtime.nseconds = 0 ;
      pentry->object.file.attributes.ctime = pentry->object.file.attributes.mtime ;
    }
  else
    {
      /* Call FSAL to actually truncate */
      pentry->object.file.attributes.asked_attributes =  pclient->attrmask ;
      fsal_status = FSAL_truncate( &pentry->object.file.handle, 
                                   pcontext, 
                                   length, 
                                   &pentry->object.file.open_fd.fd, /* Used only with FSAL_PROXY */
                                   &pentry->object.file.attributes ) ;
      
      if( FSAL_IS_ERROR( fsal_status ) ) 
        {
          *pstatus = cache_inode_error_convert( fsal_status ) ;
          if( use_mutex )
            V( pentry->lock ) ;
          
          /* stats */
          pclient->stat.func_stats.nb_err_unrecover[CACHE_INODE_TRUNCATE] += 1 ;
         
          if( fsal_status.major == ERR_FSAL_STALE ) 
            {
		cache_inode_status_t kill_status ;

		DisplayLog( "cache_inode_truncate: Stale FSAL File Handle detected for pentry = %p", pentry ) ;

 		if( cache_inode_kill_entry( pentry, ht, pclient, &kill_status ) != CACHE_INODE_SUCCESS )
                    DisplayLog( "cache_inode_truncate: Could not kill entry %p, status = %u", pentry, kill_status ) ;

                *pstatus = CACHE_INODE_FSAL_ESTALE ;
            }
 
          return *pstatus ;
        }
    }
  
  /* Validate the entry */
  *pstatus = cache_inode_valid( pentry, CACHE_INODE_OP_SET, pclient ) ;  

  /* Regular exit */
  if( use_mutex )
    V( pentry->lock ) ;
  
  /* Returns the attributes */
  *pattr = pentry->object.file.attributes ;
  

  /* stat */
  if( *pstatus != CACHE_INODE_SUCCESS )
    pclient->stat.func_stats.nb_err_retryable[CACHE_INODE_TRUNCATE] += 1 ;
  else
    pclient->stat.func_stats.nb_success[CACHE_INODE_TRUNCATE] += 1 ;
  
  return *pstatus ; 
} /* cache_inode_truncate_sw */

/**
 *
 * cache_inode_truncate_no_mutex: truncates a regular file specified by its cache entry (no mutex management).
 * 
 * Truncates a regular file specified by its cache entry.
 *
 * @param pentry    [INOUT] entry pointer for the fs object to be truncated. 
 * @param length    [IN]    wanted length for the file. 
 * @param pattr     [OUT]   attrtibutes for the file after the operation. 
 * @param ht        [INOUT] hash table used for the cache. Unused in this call (kept for protototype's homogeneity). 
 * @param pclient   [INOUT] ressource allocated by the client for the nfs management.
 * @param pcontext     [IN]    FSAL credentials 
 * @param pstatus   [OUT]   returned status.
 * 
 * @return CACHE_INODE_SUCCESS if operation is a success \n
 * @return CACHE_INODE_LRU_ERROR if allocation error occured when validating the entry
 *
 */
cache_inode_status_t cache_inode_truncate_no_mutex( cache_entry_t        * pentry, 
                                                    fsal_size_t            length,  
                                                    fsal_attrib_list_t   * pattr, 
                                                    hash_table_t         * ht, 
                                                    cache_inode_client_t * pclient, 
                                                    fsal_op_context_t          * pcontext, 
                                                    cache_inode_status_t * pstatus ) 
{
  return cache_inode_truncate_sw( pentry, 
                                  length, 
                                  pattr, 
                                  ht, 
                                  pclient, 
                                  pcontext, 
                                  pstatus, 
                                  FALSE ) ;
} /* cache_inode_truncate_no_mutex */

/**
 *
 * cache_inode_truncate: truncates a regular file specified by its cache entry.
 * 
 * Truncates a regular file specified by its cache entry.
 *
 * @param pentry    [INOUT] entry pointer for the fs object to be truncated. 
 * @param length    [IN]    wanted length for the file. 
 * @param pattr     [OUT]   attrtibutes for the file after the operation. 
 * @param ht        [INOUT] hash table used for the cache. Unused in this call (kept for protototype's homogeneity). 
 * @param pclient   [INOUT] ressource allocated by the client for the nfs management.
 * @param pcontext     [IN]    FSAL credentials 
 * @param pstatus   [OUT]   returned status.
 * 
 * @return CACHE_INODE_SUCCESS if operation is a success \n
 * @return CACHE_INODE_LRU_ERROR if allocation error occured when validating the entry
 *
 */
cache_inode_status_t cache_inode_truncate( cache_entry_t        * pentry, 
                                                    fsal_size_t            length,  
                                                    fsal_attrib_list_t   * pattr, 
                                                    hash_table_t         * ht, 
                                                    cache_inode_client_t * pclient, 
                                                    fsal_op_context_t          * pcontext, 
                                                    cache_inode_status_t * pstatus ) 
{
  return cache_inode_truncate_sw( pentry, 
                                  length, 
                                  pattr, 
                                  ht, 
                                  pclient, 
                                  pcontext, 
                                  pstatus, 
                                  TRUE ) ;
} /* cache_inode_truncate */