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
 * \file    nfs_Rename.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:53 $
 * \version $Revision: 1.16 $
 * \brief   Routines used for managing the NFS4 COMPOUND functions.
 *
 * nfs_Rename.c : Routines used for managing the NFS4 COMPOUND functions.
 *
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>           /* for having FNDELAY */
#include "HashData.h"
#include "HashTable.h"
#ifdef _USE_GSSRPC
#include <gssrpc/types.h>
#include <gssrpc/rpc.h>
#include <gssrpc/auth.h>
#include <gssrpc/pmap_clnt.h>
#else
#include <rpc/types.h>
#include <rpc/rpc.h>
#include <rpc/auth.h>
#include <rpc/pmap_clnt.h>
#endif

#include "log_functions.h"
#include "stuff_alloc.h"
#include "nfs23.h"
#include "nfs4.h"
#include "mount.h"
#include "nfs_core.h"
#include "cache_inode.h"
#include "cache_content.h"
#include "nfs_exports.h"
#include "nfs_creds.h"
#include "nfs_proto_functions.h"
#include "nfs_tools.h"
#include "nfs_proto_tools.h"

/**
 *
 * nfs_Create: The NFS PROC2 and PROC3 SYMLINK
 *
 * Implements the NFS PROC SYMLINK function (for V2 and V3).
 *
 * @param parg    [IN]    pointer to nfs arguments union
 * @param pexport [IN]    pointer to nfs export list 
 * @param pcontext   [IN]    credentials to be used for this request
 * @param pclient [INOUT] client resource to be used
 * @param ht      [INOUT] cache inode hash table
 * @param preq    [IN]    pointer to SVC request related to this call 
 * @param pres    [OUT]   pointer to the structure to contain the result of the call
 *
 * @return NFS_REQ_OK if successfull \n
 *         NFS_REQ_DROP if failed but retryable  \n
 *         NFS_REQ_FAILED if failed and not retryable.
 *
 */

int nfs_Rename(nfs_arg_t * parg /* IN  */ ,
               exportlist_t * pexport /* IN  */ ,
               fsal_op_context_t * pcontext /* IN  */ ,
               cache_inode_client_t * pclient /* IN  */ ,
               hash_table_t * ht /* INOUT */ ,
               struct svc_req *preq /* IN  */ ,
               nfs_res_t * pres /* OUT */ )
{
  static char __attribute__ ((__unused__)) funcName[] = "nfs_Rename";

  char *str_entry_name = NULL;
  fsal_name_t entry_name;
  char *str_new_entry_name = NULL;
  fsal_name_t new_entry_name;
  cache_entry_t *pentry = NULL;
  cache_entry_t *new_pentry = NULL;
  cache_entry_t *parent_pentry = NULL;
  cache_entry_t *new_parent_pentry = NULL;
  cache_entry_t *should_not_exists = NULL;
  cache_entry_t *should_exists = NULL;
  cache_inode_status_t cache_status;
  int rc;
  fsal_attrib_list_t *ppre_attr;
  fsal_attrib_list_t pre_attr;
  fsal_attrib_list_t *pnew_pre_attr;
  fsal_attrib_list_t new_attr;
  fsal_attrib_list_t new_parent_attr;
  fsal_attrib_list_t attr;
  fsal_attrib_list_t tst_attr;
  cache_inode_file_type_t parent_filetype;
  cache_inode_file_type_t new_parent_filetype;

  if(preq->rq_vers == NFS_V3)
    {
      /* to avoid setting it on each error case */
      pres->res_rename3.RENAME3res_u.resfail.fromdir_wcc.before.attributes_follow = FALSE;
      pres->res_rename3.RENAME3res_u.resfail.fromdir_wcc.after.attributes_follow = FALSE;
      pres->res_rename3.RENAME3res_u.resfail.todir_wcc.before.attributes_follow = FALSE;
      pres->res_rename3.RENAME3res_u.resfail.todir_wcc.after.attributes_follow = FALSE;
      ppre_attr = NULL;
      pnew_pre_attr = NULL;
    }

  /* Convert fromdir file handle into a cache_entry */
  if((parent_pentry = nfs_FhandleToCache(preq->rq_vers,
                                         &(parg->arg_rename2.from.dir),
                                         &(parg->arg_rename3.from.dir),
                                         NULL,
                                         &(pres->res_dirop2.status),
                                         &(pres->res_create3.status),
                                         NULL,
                                         &pre_attr, pcontext, pclient, ht, &rc)) == NULL)
    {
      /* Stale NFS FH ? */
      return rc;
    }

  /* Convert todir file handle into a cache_entry */
  if((new_parent_pentry = nfs_FhandleToCache(preq->rq_vers,
                                             &(parg->arg_rename2.to.dir),
                                             &(parg->arg_rename3.to.dir),
                                             NULL,
                                             &(pres->res_dirop2.status),
                                             &(pres->res_create3.status),
                                             NULL,
                                             &new_parent_attr,
                                             pcontext, pclient, ht, &rc)) == NULL)
    {
      /* Stale NFS FH ? */
      return rc;
    }

  /* get the attr pointers */
  ppre_attr = &pre_attr;
  pnew_pre_attr = &new_parent_attr;

  /* Get the filetypes */
  parent_filetype = cache_inode_fsal_type_convert(pre_attr.type);
  new_parent_filetype = cache_inode_fsal_type_convert(new_parent_attr.type);

  /*
   * Sanity checks: we must manage directories
   */
  if((parent_filetype != DIR_BEGINNING && parent_filetype != DIR_CONTINUE) ||
     (new_parent_filetype != DIR_BEGINNING && new_parent_filetype != DIR_CONTINUE))
    {
      switch (preq->rq_vers)
        {
        case NFS_V2:
          pres->res_stat2 = NFSERR_NOTDIR;
          break;

        case NFS_V3:
          pres->res_rename3.status = NFS3ERR_NOTDIR;
          break;
        }

      return NFS_REQ_OK;
    }

  switch (preq->rq_vers)
    {
    case NFS_V2:
      str_entry_name = parg->arg_rename2.from.name;
      str_new_entry_name = parg->arg_rename2.to.name;
      break;

    case NFS_V3:
      str_entry_name = parg->arg_rename3.from.name;
      str_new_entry_name = parg->arg_rename3.to.name;
      break;
    }

  pentry = new_pentry = NULL;

  if(str_entry_name == NULL ||
     strlen(str_entry_name) == 0 ||
     str_new_entry_name == NULL ||
     strlen(str_new_entry_name) == 0 ||
     FSAL_IS_ERROR(FSAL_str2name(str_entry_name, FSAL_MAX_NAME_LEN, &entry_name)) ||
     FSAL_IS_ERROR(FSAL_str2name(str_new_entry_name, FSAL_MAX_NAME_LEN, &new_entry_name)))
    {
      cache_status = CACHE_INODE_INVALID_ARGUMENT;
    }
  else
    {
      /*
       * Lookup file to see if new entry exists
       *
       */
      should_not_exists = cache_inode_lookup(new_parent_pentry,
                                             &new_entry_name,
                                             &tst_attr,
                                             ht, pclient, pcontext, &cache_status);

      if(cache_status == CACHE_INODE_NOT_FOUND)
        {
          /* We need to lookup over the old entry also */
          should_exists = cache_inode_lookup(parent_pentry,
                                             &entry_name,
                                             &tst_attr,
                                             ht, pclient, pcontext, &cache_status);

          /* Rename entry */
          if(cache_status == CACHE_INODE_SUCCESS)
            cache_inode_rename(parent_pentry,
                               &entry_name,
                               new_parent_pentry,
                               &new_entry_name,
                               &attr, &new_attr, ht, pclient, pcontext, &cache_status);

          if(cache_status == CACHE_INODE_SUCCESS)
            {
              switch (preq->rq_vers)
                {
                case NFS_V2:
                  pres->res_stat2 = NFS_OK;
                  break;

                case NFS_V3:
                  /*
                   * Build Weak Cache Coherency
                   * data 
                   */
                  nfs_SetWccData(pcontext, pexport,
                                 parent_pentry,
                                 ppre_attr,
                                 &attr,
                                 &(pres->res_rename3.RENAME3res_u.resok.fromdir_wcc));

                  nfs_SetWccData(pcontext, pexport,
                                 new_parent_pentry,
                                 pnew_pre_attr,
                                 &new_attr,
                                 &(pres->res_rename3.RENAME3res_u.resok.todir_wcc));

                  pres->res_rename3.status = NFS3_OK;
                  break;

                }

              return NFS_REQ_OK;
            }
        }
      else
        {
          /* If name are the same (basically renaming a/file1 to a/file1, this is a non-erroneous situation to be managed */
          if(new_parent_pentry == parent_pentry)
            {
              if(!FSAL_namecmp(&new_entry_name, &entry_name))
                {
                  /* trying to rename a file to himself, this is allowed */
                  cache_status = CACHE_INODE_SUCCESS;
                  switch (preq->rq_vers)
                    {
                    case NFS_V2:
                      pres->res_stat2 = NFS_OK;
                      break;

                    case NFS_V3:
                      /*
                       * Build Weak Cache Coherency
                       * data 
                       */
                      nfs_SetWccData(pcontext, pexport,
                                     parent_pentry,
                                     ppre_attr,
                                     &attr,
                                     &(pres->res_rename3.RENAME3res_u.resok.fromdir_wcc));

                      nfs_SetWccData(pcontext, pexport,
                                     parent_pentry,
                                     ppre_attr,
                                     &attr,
                                     &(pres->res_rename3.RENAME3res_u.resok.todir_wcc));

                      pres->res_rename3.status = NFS3_OK;
                      break;

                    }

                  return NFS_REQ_OK;
                }

            }

          /* New entry already exists. In this case (see RFC), entry should be compatible: Both are non-directories or 
           * both are directories and 'todir' is empty. If compatible, old 'todir' entry is scratched, if not returns EEXISTS */
          if(should_not_exists != NULL)
            {
              /* We need to lookup over the old entry also */
              if((should_exists = cache_inode_lookup(parent_pentry,
                                                     &entry_name,
                                                     &tst_attr,
                                                     ht,
                                                     pclient,
                                                     pcontext, &cache_status)) != NULL)
                {
                  if(cache_inode_type_are_rename_compatible
                     (should_exists, should_not_exists))
                    {
                      /* Remove the old entry before renaming it */
                      if(cache_inode_remove(new_parent_pentry,
                                            &new_entry_name,
                                            &tst_attr,
                                            ht,
                                            pclient,
                                            pcontext,
                                            &cache_status) == CACHE_INODE_SUCCESS)
                        {
                          if(cache_inode_rename(parent_pentry,
                                                &entry_name,
                                                new_parent_pentry,
                                                &new_entry_name,
                                                &attr,
                                                &new_attr,
                                                ht,
                                                pclient,
                                                pcontext,
                                                &cache_status) == CACHE_INODE_SUCCESS)
                            {
                              /* trying to rename a file to himself, this is allowed */
                              switch (preq->rq_vers)
                                {
                                case NFS_V2:
                                  pres->res_stat2 = NFS_OK;
                                  break;

                                case NFS_V3:
                                  /*
                                   * Build Weak Cache Coherency
                                   * data 
                                   */
                                  nfs_SetWccData(pcontext, pexport,
                                                 parent_pentry,
                                                 ppre_attr,
                                                 &attr,
                                                 &(pres->res_rename3.RENAME3res_u.resok.
                                                   fromdir_wcc));

                                  nfs_SetWccData(pcontext, pexport,
                                                 parent_pentry,
                                                 ppre_attr,
                                                 &attr,
                                                 &(pres->res_rename3.RENAME3res_u.resok.
                                                   todir_wcc));

                                  pres->res_rename3.status = NFS3_OK;
                                  break;

                                }

                              return NFS_REQ_OK;
                            }
                        }

                    }
                }               /*  if( cache_inode_type_are_rename_compatible( should_exists, should_not_exists ) ) */
            }

          /* if( ( should_exists = cache_inode_lookup( parent_pentry, .... */
          /* If this point is reached, then destination object already exists with that name in the directory 
             and types are not compatible, we should return that the file exists */
          cache_status = CACHE_INODE_ENTRY_EXISTS;
        }                       /* if( should_not_exists != NULL ) */
    }

  /* If we are here, there was an error */
  if(nfs_RetryableError(cache_status))
    {
      return NFS_REQ_DROP;
    }

  nfs_SetFailedStatus(pcontext, pexport,
                      preq->rq_vers,
                      cache_status,
                      &pres->res_stat2,
                      &pres->res_rename3.status,
                      NULL, NULL,
                      parent_pentry,
                      ppre_attr,
                      &(pres->res_rename3.RENAME3res_u.resfail.fromdir_wcc),
                      new_parent_pentry,
                      pnew_pre_attr, &(pres->res_rename3.RENAME3res_u.resfail.todir_wcc));

  return NFS_REQ_OK;

}                               /* nfs_Rename */

/**
 * nfs_Rename_Free: Frees the result structure allocated for nfs_Rename.
 * 
 * Frees the result structure allocated for nfs_Rename.
 * 
 * @param pres        [INOUT]   Pointer to the result structure.
 *
 */
void nfs_Rename_Free(nfs_res_t * resp)
{
  /* Nothing to do here */
  return;
}                               /* nfs_Rename_Free */