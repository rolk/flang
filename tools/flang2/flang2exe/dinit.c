/*
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/** \file
 * \brief SCFTN routine to process data initialization statements; called by
 * semant.
 */

#include "gbldefs.h"
#include "error.h"
#include "global.h"
#include "semant.h"
#include "symtab.h"
#include "ilm.h"
#include "ilmtp.h"
#include "dinit.h"
#include "machardf.h"

/** \brief Effective address of a reference being initialized */
typedef struct {
  int sptr; /**< the containing object being initialized */
  int mem;  /**< the variable or member being initialized; if not
             * a member, same as sptr.
             */
  ISZ_T offset;
} EFFADR;

static int chk_doindex();
static EFFADR *mkeffadr();
static ISZ_T eval();
extern void dmp_ivl(VAR *, FILE *);
extern void dmp_ict(CONST *, FILE *);
static char *acl_idname(int);
static char *ac_opname(int);
static void dinit_data(VAR *, CONST *, int, ISZ_T);
static void dinit_subs(CONST *, int, ISZ_T, int);
static void dinit_val();
static void sym_is_dinitd();
static LOGICAL is_zero(int, INT);
static ISZ_T get_ival(int, INT);
static INT _fdiv(INT dividend, INT divisor);
static void _ddiv(INT *dividend, INT *divisor, INT *quotient);
static CONST *eval_init_op(int, CONST *, int, CONST *, int, int, int);
static INT init_fold_const(int opr, INT lop, INT rop, int dtype);
static CONST *eval_init_expr_item(CONST *cur_e);
static CONST *eval_array_constructor(CONST *e);
static CONST *eval_init_expr(CONST *e);
static CONST *eval_do(CONST *ido);
static void replace_const(CONST *old, CONST *replacement);
static CONST *clone_init_const(CONST *original, int temp);
static CONST *clone_init_const_list(CONST *original, int temp);
static void add_to_list(CONST *val, CONST **root, CONST **tail);
static void save_init(CONST *ict, int sptr);
static void dmp_saved_init(int sptr, int save_idx);

extern LOGICAL dinit_ok();

extern void dmpilms();

static CONST **init_const = 0; /* list of pointers to saved COSNT lists */
static int cur_init = 0;
int init_list_count = 0; /* size of init_const */
static CONST const_err = {0, NULL, NULL, 0, 0, 0, 0};
#define CONST_ERR(dt) (const_err.dtype = dt, clone_init_const(&const_err, TRUE))

static int substr_len; /* length of char substring being init'd */

typedef struct {
  int sptr;
  ISZ_T currval;
  ISZ_T upbd;
  ISZ_T step;
} DOSTACK;

#define MAXDEPTH 8
static DOSTACK dostack[MAXDEPTH];
static DOSTACK *top, *bottom;

static FILE *df = NULL; /* defer dinit until semfin */

/* Define repeat value when use of REPEAT dinit records becomes worthwhile */

#define THRESHOLD 6

/*****************************************************************/

void dinit(ivl, ict)
    /*
     * Instead of creating dinit records during the processing of data
     * initializations, we need to save information so the records are written
     * at the end of semantic analysis (during semfin).  This is necessary for
     * at least a couple of reasons: 1). a record dcl with inits in its
     * STRUCTURE
     * could occur before resolution of its storage class (problematic is
     * SC_CMBLK)  2). with VMS ftn, an array may be initialized (not by implied
     * DO) before resolution of its stype (i.e., its DIMENSION).
     *
     * The information we need to save is the pointers to the var list and
     * constant tree and the ilms.  This also implies that the getitem areas
     * (4, 5) need to stay around until semfin.
     */
    VAR *ivl;
CONST *ict;
{
  int nw;
  char *ptr;
  ILM_T *p;

  if (df == NULL) {
    if ((df = tmpf("b")) == NULL)
      errfatal(5);
  }
  ptr = (char *)ivl;
  nw = fwrite(&ptr, sizeof(ivl), 1, df);
  if (nw != 1)
    error(10, 40, 0, "(data init file)", CNULL);
  ptr = (char *)ict;
  nw = fwrite(&ptr, sizeof(ict), 1, df);
  if (nw != 1)
    error(10, 40, 0, "(data init file)", CNULL);
  p = ilmb.ilm_base;
  *p++ = IM_BOS;
  *p++ = gbl.lineno;
  *p++ = gbl.findex;
  *p = ilmb.ilmavl;
  nw = fwrite((char *)ilmb.ilm_base, sizeof(ILM_T), ilmb.ilmavl, df);
  if (nw != ilmb.ilmavl)
    error(10, 40, 0, "(data init file)", CNULL);
#if DEBUG
  if (DBGBIT(6, 16)) {
    fprintf(gbl.dbgfil, "---- deferred dinit write: ivl %p, ict %p\n",
            (void *)ivl, (void *)ict);
    dumpilms();
  }
#endif

}

static void df_dinit(VAR *, CONST *);

void
do_dinit()
{
  /*
   * read in the information a "record" (2 pointers and ilms) at a time
   * saved by dinit(), and write dinit records for each record.
   */
  VAR *ivl;
  CONST *ict;
  char *ptr;
  int nw;
  int nilms;

  if (df == NULL)
    return;
  nw = fseek(df, 0L, 0);
#if DEBUG
  assert(nw == 0, "do_dinit:bad rewind", nw, 4);
#endif

  /* allocate the list of pointers to save initializer constant lists */
  init_const = (CONST **)getitem(4, init_list_count * sizeof(CONST *));
  BZERO(init_const, sizeof(CONST *), init_list_count);

  while (TRUE) {
    nw = fread(&ptr, sizeof(ivl), 1, df);
    if (nw == 0)
      break;
#if DEBUG
    assert(nw == 1, "do_dinit: ict error", nw, 4);
#endif
    ivl = (VAR *)ptr;
    nw = fread(&ptr, sizeof(ict), 1, df);
#if DEBUG
    assert(nw == 1, "do_dinit: ivl error", nw, 4);
#endif
    ict = (CONST *)ptr;
    nw = fread((char *)ilmb.ilm_base, sizeof(ILM_T), BOS_SIZE, df);
#if DEBUG
    assert(nw == BOS_SIZE, "do_dinit: BOS error", nw, 4);
#endif
    /*
     * determine the number of words remaining in the ILM block
     */
    nilms = *(ilmb.ilm_base + 3);
    nw = nilms - BOS_SIZE;

    /* read in the remaining part of the ILM block  */

    nilms = fread((char *)(ilmb.ilm_base + BOS_SIZE), sizeof(ILM_T), nw, df);
#if DEBUG
    assert(nilms == nw, "do_dinit: BLOCK error", nilms, 3);
#endif
    gbl.lineno = ilmb.ilm_base[1];
    gbl.findex = ilmb.ilm_base[2];
    ilmb.ilmavl = ilmb.ilm_base[3];
#if DEBUG
    if (DBGBIT(6, 32)) {
      fprintf(gbl.dbgfil, "---- deferred dinit read: ivl %p, ict %p\n",
              (void *)ivl, (void *)ict);
    }
#endif
    if (ict && ict->no_dinitp &&
        (SCG(ict->sptr) == SC_LOCAL || SCG(ict->sptr) == SC_PRIVATE))
      continue;
    df_dinit(ivl, ict);
  }

  fclose(df);
  df = NULL;
  freearea(5);

}

/**
 * \param ivl pointer to initializer variable list
 * \param ict pointer to initializer constant tree
 */
static void
df_dinit(VAR *ivl, CONST *ict)
{
  CONST *new_ict;
#if DEBUG
  if (DBGBIT(6, 3)) {
    fprintf(gbl.dbgfil, "\nDINIT CALLED ----------------\n");
    if (DBGBIT(6, 2)) {
      if (ivl) {
        fprintf(gbl.dbgfil, "  Dinit Variable List:\n");
        dmp_ivl(ivl, gbl.dbgfil);
      }
      if (ict) {
        fprintf(gbl.dbgfil, "  Dinit Constant List:\n");
        dmp_ict(ict, gbl.dbgfil);
      }
    }
  }
#endif

  substr_len = 0;

  new_ict = eval_init_expr(ict);
#if DEBUG
  if (DBGBIT(6, 2)) {
    if (new_ict) {
      fprintf(gbl.dbgfil, "  Dinit new_Constant List:\n");
      dmp_ict(new_ict, gbl.dbgfil);
    }
  }
  if (DBGBIT(6, 1))
    fprintf(gbl.dbgfil, "\n  DINIT Records:\n");
#endif
  if (ivl) {
    bottom = top = &dostack[0];
    dinit_data(ivl, new_ict, 0, 0); /* Process DATA statements */
  } else {
    sym_is_dinitd((int)ict->sptr);
    dinit_subs(new_ict, ict->sptr, 0, 0); /* Process type dcl inits and */
  }                                       /* init'ed structures */

#if DEBUG
  if (DBGBIT(6, 3))
    fprintf(gbl.dbgfil, "\nDINIT RETURNING ----------------\n\n");
#endif
}

static CONST *
dinit_varref(VAR *ivl, int member, CONST *ict, int dtype,
             int *struct_bytes_initd, ISZ_T *repeat, ISZ_T base_off)
{
  int sptr;     /* containing object being initialized */
  int init_sym; /* member or variable being initialized */
  ISZ_T offset, elsize, num_elem, i;
  LOGICAL new_block; /* flag to put out DINIT record */
  EFFADR *effadr;    /* Effective address of array ref */
  LOGICAL zero;      /* is this put DINIT_ZEROES? */
  CONST *saved_ict;
  CONST *param_ict;
  LOGICAL put_value = TRUE;
  int ilmptr;

  if (ivl && ivl->u.varref.id == S_IDENT) {
    /* We are dealing with a scalar or whole array init */
    ilmptr = ivl->u.varref.ptr;
    /*
     * DINITPOINTER23995 - when POINTER dinits are passed thru, the reference
     * ILM  will be a IM_PLD -- its operand is an IM_BASE.
     */
    if (ILMA(ilmptr) == IM_PLD)
      ilmptr = ILMA(ilmptr+1);
    assert(ILMA(ilmptr) == IM_BASE, "dinit_data not IM_BASE", ilmptr, 3);
    init_sym = sptr = ILMA(ilmptr + 1);
    if (!dinit_ok(sptr))
      goto error_exit;
    num_elem = 1;
    offset = 0;
    if (!POINTERG(sptr) && DTY(DTYPEG(sptr)) == TY_ARRAY) {
      /* A whole array so determine number of elements to init */
      if (extent_of(DTYPEG(sptr)))
        num_elem = ad_val_of(AD_NUMELM(AD_PTR(sptr)));
      else
        num_elem = 0;
      if (num_elem == 0)
        elsize = size_of((int)DTYPEG(sptr));
      else
        elsize = size_of((int)DTYPEG(sptr)) / num_elem;
    }
  } else if (member) {
    init_sym = sptr = member;
    num_elem = 1;
    offset = ADDRESSG(sptr) + base_off;
    elsize = size_of((int)DTYPEG(sptr));
    if (!POINTERG(sptr) && DTY(DTYPEG(sptr)) == TY_ARRAY) {
      /* A whole array so determine number of elements to init */
      if (extent_of(DTYPEG(sptr)))
        num_elem = ad_val_of(AD_NUMELM(AD_PTR(sptr)));
      else
        num_elem = 0;
      if (num_elem == 0)
        elsize = size_of((int)DTYPEG(sptr));
      else
        elsize = size_of((int)DTYPEG(sptr)) / num_elem;
    }
  } else {
    /* We are dealing with an array element, array slice,
     * character substr_len, or derived type member init.
     */
    /* First dereference the ilm ptr to a symbol pointer */
    effadr = mkeffadr(ivl->u.varref.ptr);
    if (sem.dinit_error)
      goto error_exit;
    if (ivl->u.varref.shape != 0)
      uf("array section");
    sptr = effadr->sptr;
    num_elem = 1;
    offset = effadr->offset;
    elsize = 1; /* doesn't matter since num_elem is 1 */
    init_sym = effadr->mem;
    if (sptr != init_sym && DTY(DTYPEG(init_sym)) == TY_ARRAY &&
        ILMA(ivl->u.varref.ptr) != IM_ELEMENT) {
      /* A whole array so determine number of elements to init */
      num_elem = ad_val_of(AD_NUMELM(AD_PTR(init_sym)));
      if (num_elem == 0)
        elsize = size_of((int)DTYPEG(sptr));
      else
        elsize = size_of((int)DTYPEG(init_sym)) / num_elem;
    }
  }

  /*  now process enough dinit constant list items to
      take care of the current varref item:  */
  new_block = TRUE;
  saved_ict = ict;

/* if this symbol is defined in an outer scope or
 *    the symbol is a member of a common block
      not defined in this procedure (i.e., DINITG not set)
 *  then plug the symbol table with the initializer list but
 *   don't write the values to the dinit file becasue it has already been done
 */
  if (UPLEVELG(sptr) || (SCG(sptr) == SC_CMBLK && !DINITG(sptr))) {
    put_value = FALSE;
  }

  if (ict && *repeat == 0) {
    *repeat = ict->repeatc;
  }
  do {
    if (no_data_components(DDTG(DTYPEG(sptr)))) {
      break;
    }
    if (ict == NULL) {
      errsev(66);
      goto error_exit;
    }

    if (ict->id == AC_ACONST) {
      *repeat = 0;
      (void)dinit_varref(ivl, member, ict->subc, dtype, struct_bytes_initd,
                         repeat, base_off);
      i = *repeat = ad_val_of(AD_NUMELM(AD_DPTR(ict->dtype)));
    } else {
      if (ivl && DTY(DDTG(ivl->u.varref.dtype)) == TY_STRUCT) {
        if (put_value) {
          if (base_off == 0) {
            dinit_put(DINIT_LOC, (ISZ_T)sptr);
          }
          if (DTY(DTYPEG(sptr)) == TY_ARRAY && offset) {
            dinit_put(DINIT_OFFSET, offset);
            dinit_data(NULL, ict->subc, ict->dtype, 0);
          } else {
            dinit_data(NULL, ict->subc, ict->dtype, offset);
          }
        }
        i = 1;
        new_block = TRUE;
      } else if (member && DTY(ict->dtype) == TY_STRUCT) {
        if (put_value) {
          dinit_data(NULL, ict->subc, ict->dtype, offset);
        }
        i = 1;
        new_block = TRUE;
      } else {
        /* if there is a repeat count in the data item list,
         * only use as many as in this array */
        i = (num_elem < *repeat) ? num_elem : *repeat;
        if (i < THRESHOLD)
          i = 1;
        if (ivl == NULL && member)
          i = 1;
        zero = FALSE;
        if (put_value) {
          if (new_block || i != 1) {
            if (!member)
              dinit_put(DINIT_LOC, (ISZ_T)sptr);
            if (offset)
              dinit_put(DINIT_OFFSET, offset);
            if (i != 1) {
              if (i > 1 && is_zero(ict->dtype, ict->u1.conval)) {
                dinit_put(DINIT_ZEROES, i * elsize);
                zero = TRUE;
              } else {
                dinit_put(DINIT_REPEAT, (ISZ_T)i);
              }
              new_block = TRUE;
            } else {
              new_block = FALSE;
            }
          }
          if (!zero) {
            if (DTY(ict->dtype) == TY_STRUCT) {
              dinit_data(NULL, ict->subc, ict->dtype, base_off);
            } else {
              dinit_val(init_sym, ict->dtype, ict->u1.conval);
            }
          }
        }
      }
    }
    offset += i * elsize;
    num_elem -= i;
    *repeat -= i;
    if (*repeat == 0) {
      ict = ict->next;
      *repeat = ict ? ict->repeatc : 0;
    }
  } while (num_elem > 0);
  if (put_value) {
    sym_is_dinitd(sptr);
  }

  if ((!member && PARAMG(sptr)) || (CCSYMG(sptr) && DINITG(sptr))) {
    /* this variable may be used in other initializations,
     * save its initializier list
     */
    save_init(clone_init_const_list(saved_ict, FALSE), sptr);
  }

  return ict;

error_exit:
  sem.dinit_error++;
  return NULL;
}

/** \brief Initialize a data object
 *
 * \param ivl   pointer to initializer variable list
 * \param ict   pointer to initializer constant tree
 * \param dtype data type of structure type, if a struct init
 */
static void
dinit_data(VAR *ivl, CONST *ict, int dtype, ISZ_T base_off)
{
  int sptr; /* containing object being initialized */
  int member;
  int struct_bytes_initd; /* use to determine fields in typedefs need
                           * to be padded */
  ILM_T *p;
  int init_sym;      /* member or variable being initialized */
  LOGICAL new_block; /* flag to put out DINIT record */
  EFFADR *effadr;    /* Effective address of array ref */
  LOGICAL zero;      /* is this put DINIT_ZEROES? */
  CONST *saved_ict;
  CONST *param_ict;
  ISZ_T repeat;

  member = 0;
  repeat = 0;

  if (ivl == NULL && dtype) {
    member = DTY(DDTG(dtype) + 1);
    if (POINTERG(member)) {
      /* get to <ptr>$p */
      member = SYMLKG(member);
    }
    struct_bytes_initd = 0;
  }

  do {
    if (member) {
      if (POINTERG(member)) {
        /* get to <ptr>$p */
        member = SYMLKG(member);
      }
      if (is_empty_typedef(DTYPEG(member))) {
        member = SYMLKG(member);
        if (member == NOSYM)
          member = 0;
      }
    }
    if ((ivl && ivl->id == Varref) || member) {
      if (member && (CLASSG(member) && VTABLEG(member) &&
                     (TBPLNKG(member) || FINALG(member)))) {
        member = SYMLKG(member);
        if (member == NOSYM)
          member = 0;
        continue;
      } else
        ict = dinit_varref(ivl, member, ict, dtype, &struct_bytes_initd,
                           &repeat, base_off);
    } else if (ivl->id == Dostart) {
      if (top == &dostack[MAXDEPTH]) {
        /*  nesting maximum exceeded.  */
        errsev(34);
        return;
      }
      top->sptr = chk_doindex(ivl->u.dostart.indvar);
      if (top->sptr == 1)
        return;
      top->currval = eval(ivl->u.dostart.lowbd);
      top->upbd = eval(ivl->u.dostart.upbd);
      top->step = eval(ivl->u.dostart.step);

      if ((top->step > 0 && top->currval > top->upbd) ||
          (top->step <= 0 && top->currval < top->upbd)) {
        VAR *wivl;
        for (wivl = ivl; wivl->id != Doend && wivl->u.doend.dostart != ivl;
             wivl = wivl->next)
          ;

        ivl = wivl;
      } else {
        ++top;
      }
    } else {
      assert(ivl->id == Doend, "dinit:badid", 0, 3);

      --top;
      top->currval += top->step;
      if ((top->step > 0 && top->currval <= top->upbd) ||
          (top->step <= 0 && top->currval >= top->upbd)) {
        /*  go back to start of this do loop  */
        ++top;
        ivl = ivl->u.doend.dostart;
      }
    }
    if (sem.dinit_error)
      goto error_exit;
    if (ivl)
      ivl = ivl->next;
    if (member) {
      struct_bytes_initd += size_of((int)DTYPEG(member));
      member = SYMLKG(member);
      if (POINTERG(member)) {
        /* get to <ptr>$p */
        member = SYMLKG(member);
      }
      if (member == NOSYM)
        member = 0;
    }
  } while (ivl || member);

/* Too many initializer is allowed.
if (ict)   errsev(67);
 */
error_exit:
#if DEBUG
  if (ivl && DBGBIT(6, 2) && ilmb.ilmavl != BOS_SIZE) {
    /* dump ilms afterwards because dmpilms overwrites opcodes */
    *(p = ilmb.ilm_base) = IM_BOS;
    *++p = gbl.lineno;
    *++p = gbl.findex;
    *++p = ilmb.ilmavl;
    dmpilms();
  }
#endif
}

/**
 * \param ict      pointer to initializer constant tree
 * \param base     sym pointer to base address
 * \param boffset  current offset from base
 * \param mbr_sptr sptr of member if processing typedef
 */
static void
dinit_subs(CONST *ict, int base, ISZ_T boffset, int mbr_sptr)
{
  ISZ_T loffset = 0; /*offset from begin of current structure */
  ISZ_T roffset = 0; /* offset from begin of member (based on repeat count) */
  ISZ_T toffset = 0; /* temp offset of for roffset, set it back to previous
                        roffset after dinit_subs call */
  int sptr;          /* symbol ptr to identifier to get initialized */
  int sub_sptr;      /* sym ptr to nested type/struct fields */
  ISZ_T i;
  int dtype;         /* data type of member being initialized */
  ISZ_T elsize = 0;  /* size of basic or array element in bytes */
  ISZ_T num_elem;    /* if handling an array, number of array elements else 1 */
  LOGICAL new_block; /* flag indicating need for DINIT_LOC record.  Always
                      * needed after a DINIT_REPEAT block */

  /*
   * We come into this routine to follow the ict links for a substructure.
   * 'boffset' comes in as the offset from the beginning of the parent
   * structure for the structure we are going to traverse.
   *
   * We have two other offsets while traversing this structure.  'loffset'
   * is the local offset from the beginning of this structure.  'roffset'
   * is the offset based on repeat counts.
   */
  new_block = TRUE;
  while (ict) {
    if (ict->subc) {
      /* Follow substructure down before continuing at this level */
      roffset = 0;
      loffset = 0;
      num_elem = 1;
      if (ict->id == AC_SCONST) {
        if (ict->sptr) {
          sub_sptr = DTY(DDTG(DTYPEG(ict->sptr)) + 1);
          if (mbr_sptr) {
            loffset = ADDRESSG(ict->sptr);
          }
        } else if (mbr_sptr) {
          dtype = DDTG(DTYPEG(mbr_sptr));
          sub_sptr = DTY(dtype) == TY_STRUCT ? DTY(DDTG(DTYPEG(mbr_sptr)) + 1)
                                             : mbr_sptr;
          loffset = ADDRESSG(mbr_sptr);
          if (DTY(DTYPEG(mbr_sptr)) == TY_ARRAY) {
            num_elem = ad_val_of(AD_NUMELM(AD_DPTR(DTYPEG(mbr_sptr))));
          }
        } else {
          interr("dinit_subs: malformed derived type init,"
                 " unable to determine member for",
                 base, 3);
          return;
        }
      } else if (ict->id == AC_ACONST) {
        if (ict->sptr) {
          sub_sptr = ict->sptr;
        } else if (mbr_sptr) {
          sub_sptr = mbr_sptr;
        } else {
          interr("dinit_subs: malformed  array init,"
                 " unable to determine member for",
                 base, 3);
          return;
        }
      } else {
        sub_sptr = 0;
      }

      /* per flyspray 15963, the roffset must be set back to its value
       * before a call to dinit_subs in for loop.
       */
      toffset = roffset;
      for (i = ict->repeatc; i != 0; i--) {
        dinit_subs(ict->subc, base, boffset + loffset + roffset, sub_sptr);
        roffset += DTY(ict->dtype + 2);
      }
      roffset = toffset;
      num_elem -= ict->repeatc;
      ict = ict->next;
      new_block = TRUE;
    } else {
      /* Handle basic type declaration init statement */
      /* If new member or member has a repeat start a new block */
      if (ict->sptr) {
        /* A new member to initialize */
        sptr = ict->sptr;
        roffset = 0;
        loffset = ADDRESSG(sptr);
        dtype = (DTYPEG(sptr));
        elsize = size_of(dtype);
        if (DTY(dtype) == TY_ARRAY)
          elsize /= ad_val_of(AD_NUMELM(AD_PTR(sptr)));
        new_block = TRUE;
      } else {
        if (ict->repeatc > 1) {
          new_block = TRUE;
        }
        if (mbr_sptr) {
          sptr = mbr_sptr;
          dtype = (DTYPEG(sptr));
          loffset = ADDRESSG(mbr_sptr);
          roffset = 0;
          elsize = size_of(dtype);
          if (DTY(dtype) == TY_ARRAY)
            elsize /= ad_val_of(AD_NUMELM(AD_PTR(sptr)));
        }
      }
      if (new_block) {
        dinit_put(DINIT_LOC, (ISZ_T)base);
        dinit_put(DINIT_OFFSET, boffset + loffset + roffset);
        new_block = FALSE;
      }
      if (ict->repeatc > 1) {
        new_block = TRUE;
        dinit_put(DINIT_REPEAT, (ISZ_T)ict->repeatc);
        num_elem = 1;
      } else {
        num_elem =
            (DTY(dtype) == TY_ARRAY) ? ad_val_of(AD_NUMELM(AD_DPTR(dtype))) : 1;
      }
      roffset += elsize * ict->repeatc;

      do {
        dinit_val(sptr, ict->dtype, ict->u1.conval);
        ict = ict->next;
      } while (--num_elem > 0);
    }
    if (ict && mbr_sptr) {
      if (ict->sptr) {
        mbr_sptr = ict->sptr;
      } else if (num_elem <= 0) {
        mbr_sptr = SYMLKG(mbr_sptr);
      }
      if (mbr_sptr == NOSYM) {
        mbr_sptr = 0;
      } else {
        new_block = TRUE;
      }
    }
  } /* End of while */
}

/*****************************************************************/
/* dinit_val - make sure constant value is correct data type to initialize
 * symbol (sptr) to.  Then call dinit_put to generate dinit record.
 */
static void dinit_val(sptr, dtypev, val) int sptr, dtypev;
INT val;
{
  int dtype;
  char buf[2];

  dtype = DDTG(DTYPEG(sptr));
  if (no_data_components(dtype)) {
    return;
  }

  if (substr_len) {
/*
 * since substr_len is non-zero, it was specified in a substring
 * operation; dtype is modified to reflect this length instead
 * of the symbol's declared length.
 */
    dtype = DTY(dtype);
    assert(dtype == TY_CHAR || dtype == TY_NCHAR, "dinit_val:nonchar sym", sptr,
           3);
    dtype = get_type(2, dtype, substr_len);
    substr_len = 0;
  }

  if (DTYG(dtypev) == TY_HOLL) {
    /* convert hollerith character string to one of proper length */
    val = cngcon(val, (int)DTYPEG(val), dtype);
  } else if (DTYG(dtypev) == TY_CHAR || DTYG(dtypev) == TY_NCHAR ||
             DTYG(dtypev) != DTY(dtype)) {
    /*  check for special case of initing character*1 to  numeric. */
    if (DTY(dtype) == TY_CHAR && DTY(dtype + 1) == 1) {
      if (DT_ISINT(dtypev) && !DT_ISLOG(dtypev)) {
        if (flg.standard)
          error(172, 2, gbl.lineno, SYMNAME(sptr), CNULL);
        if (val < 0 || val > 255) {
          error(68, 3, gbl.lineno, SYMNAME(sptr), CNULL);
          val = getstring(" ", 1);
        } else {
          buf[0] = (char)val;
          buf[1] = 0;
          val = getstring(buf, 1);
        }
        dtypev = DT_CHAR;
      }
    }
    /* Convert character string to one of proper length or,
     * convert constant to type of identifier
     */
    val = cngcon(val, dtypev, dtype);
  }
  dinit_put(dtype, val);

  if (flg.opt >= 2 && STYPEG(sptr) == ST_VAR && SCG(sptr) == SC_LOCAL) {
    NEED(aux.dvl_avl + 1, aux.dvl_base, DVL, aux.dvl_size, aux.dvl_size + 32);
    DVL_SPTR(aux.dvl_avl) = sptr;
    DVL_CONVAL(aux.dvl_avl) = val;
    REDUCP(sptr, 1); /* => in dvl table */
    aux.dvl_avl++;
  }

}

/*****************************************************************/

void
dmp_ivl(VAR *ivl, FILE *f)
{
  FILE *dfil;
  dfil = f ? f : stderr;
  while (ivl) {
    if (ivl->id == Dostart) {
      fprintf(dfil, "    Do begin marker  (0x%p):", (void *)ivl);
      fprintf(dfil, " indvar: %4d lowbd:%4d", ivl->u.dostart.indvar,
              ivl->u.dostart.lowbd);
      fprintf(dfil, " upbd:%4d  step:%4d\n", ivl->u.dostart.upbd,
              ivl->u.dostart.step);
    } else if (ivl->id == Varref) {
      fprintf(dfil, "    Variable reference (");
      if (ivl->u.varref.id == S_IDENT) {
        fprintf(dfil, " S_IDENT):");
        fprintf(dfil, " sptr: %d(%s)", ILMA(ivl->u.varref.ptr + 1),
                SYMNAME(ILMA(ivl->u.varref.ptr + 1)));
        fprintf(dfil, " dtype: %4d\n", DTYPEG(ILMA(ivl->u.varref.ptr + 1)));
      } else {
        fprintf(dfil, "S_LVALUE):");
        fprintf(dfil, "  ilm:%4d", ivl->u.varref.ptr);
        fprintf(dfil, " shape:%4d\n", ivl->u.varref.shape);
      }
    } else {
      assert(ivl->id == Doend, "dmp_ivl: badid", 0, 3);
      fprintf(dfil, "    Do end marker:");
      fprintf(dfil, "   Pointer to Do Begin: %p\n",
              (void *)ivl->u.doend.dostart);
    }
    ivl = ivl->next;
  }
}

void
dmp_ict(CONST *ict, FILE *f)
{
  FILE *dfil;
  dfil = f ? f : stderr;
  while (ict) {
    fprintf(dfil, "%p(%s):", (void *)ict, acl_idname(ict->id));
    if (ict->subc) {
      fprintf(dfil, "  subc: for structure tag %s  ",
              SYMNAME(DTY(ict->dtype + 3)));
      fprintf(dfil, "  sptr: %d", ict->sptr);
      if (ict->sptr) {
        fprintf(dfil, "(%s)", SYMNAME(ict->sptr));
      }
      fprintf(dfil, "  mbr: %d", ict->mbr);
      if (ict->mbr) {
        fprintf(dfil, "(%s)", SYMNAME(ict->mbr));
      }
      fprintf(dfil, "  rc: %" ISZ_PF "d", ict->repeatc);
      /*fprintf(dfil, "  next:%p\n", (void *)(ict->next));*/
      fprintf(dfil, "\n");
      dmp_ict(ict->subc, f);
      fprintf(dfil, "    Back from most recent substructure %p\n", ict);
      ict = ict->next;
    } else {
      fprintf(dfil, "  val: %6d   dt: %4d   rc: %6" ISZ_PF "d", ict->u1.conval,
              ict->dtype, ict->repeatc);
      fprintf(dfil, "  sptr: %d", ict->sptr);
      if (ict->sptr) {
        fprintf(dfil, "(%s)", SYMNAME(ict->sptr));
      }
      fprintf(dfil, "  mbr: %d", ict->mbr);
      if (ict->mbr) {
        fprintf(dfil, "(%s)", SYMNAME(ict->mbr));
      }
      /*fprintf(dfil, "  next:%p\n", (void *)(ict->next));*/
      fprintf(dfil, "\n");
      ict = ict->next;
    }
  }
}

static char *
acl_idname(int id)
{
  static char bf[32];
  switch (id) {
  case AC_IDENT:
    strcpy(bf, "IDENT");
    break;
  case AC_CONST:
    strcpy(bf, "CONST");
    break;
  case AC_EXPR:
    strcpy(bf, "EXPR");
    break;
  case AC_IEXPR:
    strcpy(bf, "IEXPR");
    break;
  case AC_AST:
    strcpy(bf, "AST");
    break;
  case AC_IDO:
    strcpy(bf, "IDO");
    break;
  case AC_REPEAT:
    strcpy(bf, "REPEAT");
    break;
  case AC_ACONST:
    strcpy(bf, "ACONST");
    break;
  case AC_SCONST:
    strcpy(bf, "SCONST");
    break;
  case AC_LIST:
    strcpy(bf, "LIST");
    break;
  case AC_VMSSTRUCT:
    strcpy(bf, "VMSSTRUCT");
    break;
  case AC_VMSUNION:
    strcpy(bf, "VMSUNION");
    break;
  case AC_TYPEINIT:
    strcpy(bf, "TYPEINIT");
    break;
  case AC_ICONST:
    strcpy(bf, "ICONST");
    break;
  case AC_CONVAL:
    strcpy(bf, "CONVAL");
    break;
  case AC_TRIPLE:
    strcpy(bf, "TRIPLE");
    break;
  default:
    sprintf(bf, "UNK_%d", id);
    break;
  }
  return bf;
}

static char *
ac_opname(int id)
{
  static char bf[32];
  switch (id) {
  case AC_ADD:
    strcpy(bf, "ADD");
    break;
  case AC_SUB:
    strcpy(bf, "SUB");
    break;
  case AC_MUL:
    strcpy(bf, "MUL");
    break;
  case AC_DIV:
    strcpy(bf, "DIV");
    break;
  case AC_NEG:
    strcpy(bf, "NEG");
    break;
  case AC_EXP:
    strcpy(bf, "EXP");
    break;
  case AC_INTR_CALL:
    strcpy(bf, "INTR_CALL");
    break;
  case AC_ARRAYREF:
    strcpy(bf, "ARRAYREF");
    break;
  case AC_MEMBR_SEL:
    strcpy(bf, "MEMBR_SEL");
    break;
  case AC_CONV:
    strcpy(bf, "CONV");
    break;
  case AC_CAT:
    strcpy(bf, "CAT");
    break;
  case AC_EXPK:
    strcpy(bf, "EXPK");
    break;
  case AC_LEQV:
    strcpy(bf, "LEQV");
    break;
  case AC_LNEQV:
    strcpy(bf, "LNEQV");
    break;
  case AC_LOR:
    strcpy(bf, "LOR");
    break;
  case AC_LAND:
    strcpy(bf, "LAND");
    break;
  case AC_EQ:
    strcpy(bf, "EQ");
    break;
  case AC_GE:
    strcpy(bf, "GE");
    break;
  case AC_GT:
    strcpy(bf, "GT");
    break;
  case AC_LE:
    strcpy(bf, "LE");
    break;
  case AC_LT:
    strcpy(bf, "LT");
    break;
  case AC_NE:
    strcpy(bf, "NE");
    break;
  case AC_LNOT:
    strcpy(bf, "LNOT");
    break;
  case AC_EXPX:
    strcpy(bf, "EXPX");
    break;
  case AC_TRIPLE:
    strcpy(bf, "TRIPLE");
    break;
  default:
    sprintf(bf, "ac_opnameUNK_%d", id);
    break;
  }
  return bf;
}

/*****************************************************************/
/* mkeffadr - derefence an ilm pointer to determine the effective address
 *            of a reference (i.e. base sptr + byte offset).
 */

static EFFADR *mkeffadr(ilmptr) int ilmptr;
{
  int opr1;
  int opr2;
  EFFADR *effadr;
  ADSC *ad;          /* Ptr to array descriptor */
  static EFFADR buf; /* Area ultimately returned containing effective addr */
  int i;
  ISZ_T offset, totoffset;
  ISZ_T lwbd;

  opr1 = ILMA(ilmptr + 1);
  opr2 = ILMA(ilmptr + 2);

  switch (ILMA(ilmptr)) {
  case IM_SUBS:
  case IM_NSUBS:
    effadr = mkeffadr(opr1);
    if (sem.dinit_error)
      break;
    lwbd = eval(opr2);
    if (ILMA(ilmptr) == IM_NSUBS) /* NCHAR/kanji - 2 bytes per char */
      effadr->offset += 2 * (lwbd - 1);
    else
      effadr->offset += lwbd - 1;
    /*  for kanji, substr_len in units of chars, not bytes: */
    substr_len = eval((int)ILMA(ilmptr + 3)) - lwbd + 1;
    break;

  case IM_ELEMENT:
    effadr = mkeffadr(opr2);
    if (sem.dinit_error)
      break;
    ad = AD_PTR(effadr->mem);
    totoffset = 0;
    for (i = 0; i < opr1; i++) {
      lwbd = ad_val_of(AD_LWBD(ad, i));
      offset = eval(ILMA(ilmptr + 4 + i));
      if (offset < lwbd || offset > ad_val_of(AD_UPBD(ad, i))) {
        error(80, 3, gbl.lineno, SYMNAME(effadr->sptr), CNULL);
        sem.dinit_error = TRUE;
        break;
      }
      totoffset += (offset - lwbd) * ad_val_of(AD_MLPYR(ad, i));
    }
    /* Convert array element offset to a byte offset */
    totoffset *= size_of((int)DDTG(DTYPEG(effadr->mem)));
    effadr->offset += totoffset;
    break;

  case IM_BASE:
    effadr = &buf;
    if (!dinit_ok(opr1))
      break;
    effadr->sptr = opr1;
    effadr->mem = opr1;
    effadr->offset = 0;
    break;

  case IM_MEMBER:
    effadr = mkeffadr(opr1);
    if (sem.dinit_error)
      break;
    effadr->mem = opr2;
    effadr->offset += ADDRESSG(opr2);
    break;

  case IM_IFUNC:
  case IM_KFUNC:
  case IM_RFUNC:
  case IM_DFUNC:
  case IM_CFUNC:
  case IM_CDFUNC:
  case IM_CALL:
    effadr = &buf;
    effadr->sptr = opr2;
    effadr->mem = opr2;
    error(76, 3, gbl.lineno, SYMNAME(effadr->sptr), CNULL);
    sem.dinit_error = TRUE;
    break;

  default:
    effadr = &buf;
    effadr->sptr = 0;
    effadr->mem = 0;
    effadr = &buf;
    sem.dinit_error = TRUE;
    break;
  }
  return effadr;
}

/*****************************************************************/

static int chk_doindex(ilmptr)
    /*
     * find the sptr for the implied do index variable; the ilm in this
     * context represents the ilms generated to load the index variable
     * and perhaps "type" convert (if it's integer*2, etc.).
     */
    int ilmptr;
{
  int sptr;
again:
  switch (ILMA(ilmptr)) {
  case IM_I8TOI:
  case IM_STOI:
  case IM_SCTOI:
    ilmptr = ILMA(ilmptr + 1);
    goto again;
  case IM_KLD:
  case IM_ILD:
  case IM_SILD:
  case IM_CHLD:
    /* find BASE of load, and then sptr of BASE */
    sptr = ILMA(ILMA(ilmptr + 1) + 1);
    return sptr;
  }
  /* could use a better error message - illegal implied do index variable */
  errsev(106);
  sem.dinit_error = TRUE;
  return 1L;
}

static ISZ_T eval(ilmptr) int ilmptr;
{
  int opr1 = ILMA(ilmptr + 1);
  int opr2;
  DOSTACK *p;

  switch (ILMA(ilmptr) /* opc */) {
  case IM_KLD:
  case IM_ILD:
  case IM_SILD:
  case IM_CHLD:
    /*  see if this ident is an active do index variable: */
    opr1 = ILMA(opr1 + 1); /* get sptr from BASE ilm */
    for (p = bottom; p < top; p++)
      if (p->sptr == opr1)
        return p->currval;
    /*  else - illegal use of variable: */
    error(64, 3, gbl.lineno, SYMNAME(opr1), CNULL);
    sem.dinit_error = TRUE;
    return 1L;

  case IM_KCON:
    return get_isz_cval(opr1);

  case IM_ICON:
    return CONVAL2G(opr1);

  case IM_KNEG:
  case IM_INEG:
    return -eval(opr1);
  case IM_KADD:
  case IM_IADD:
    return eval(opr1) + eval(ILMA(ilmptr + 2));
  case IM_KSUB:
  case IM_ISUB:
    return eval(opr1) - eval(ILMA(ilmptr + 2));
  case IM_KMUL:
  case IM_IMUL:
    return eval(opr1) * eval(ILMA(ilmptr + 2));
  case IM_KDIV:
  case IM_IDIV:
    return eval(opr1) / eval(ILMA(ilmptr + 2));
  case IM_ITOI8:
  case IM_I8TOI:
  case IM_STOI:
  case IM_SCTOI:
    /* these should reference SILD/CHLD */
    return eval(opr1);

  default:
    errsev(69);
    sem.dinit_error = TRUE;
    return 1L;
  }
}

/** \brief Return TRUE if the constant of the given dtype represents zero */
static LOGICAL
is_zero(int dtype, INT conval)
{
  switch (DTY(dtype)) {
  case TY_INT8:
  case TY_LOG8:
    if (CONVAL2G(conval) == 0 && (!XBIT(124, 0x400) || CONVAL1G(conval) == 0))
      return TRUE;
    break;
  case TY_INT:
  case TY_LOG:
  case TY_SINT:
  case TY_SLOG:
  case TY_BINT:
  case TY_BLOG:
  case TY_FLOAT:
    if (conval == 0)
      return TRUE;
    break;
  case TY_DBLE:
    if (conval == stb.dbl0)
      return TRUE;
    break;
  case TY_CMPLX:
    if (CONVAL1G(conval) == 0 && CONVAL2G(conval) == 0)
      return TRUE;
    break;
  case TY_DCMPLX:
    if (CONVAL1G(conval) == stb.dbl0 && CONVAL2G(conval) == stb.dbl0)
      return TRUE;
    break;
  default:
    break;
  }
  return FALSE;
}

static ISZ_T
get_ival(int dtype, INT conval)
{
  switch (DTY(dtype)) {
  case TY_INT8:
  case TY_LOG8:
    return get_isz_cval(conval);
  default:
    break;
  }
  return conval;
}

/*****************************************************************/
/*
 * sym_is_dinitd: a symbol is being initialized - update certain
 * attributes of the symbol including its dinit flag.
 */
static void sym_is_dinitd(sptr) int sptr;
{
  DINITP(sptr, 1);
  if (SCG(sptr) == SC_CMBLK)
    /*  set DINIT flag for common block:  */
    DINITP(MIDNUMG(sptr), 1);

  /* For identifiers the DATA statement ensures that the identifier
   * is a variable and not an intrinsic.  For arrays, either
   * compute the element offset or if a whole array reference
   * compute the number of elements to initialize.
   */
  if (STYPEG(sptr) == ST_IDENT || STYPEG(sptr) == ST_UNKNOWN)
    STYPEP(sptr, ST_VAR);

}

/*****************************************************************/

LOGICAL
dinit_ok(sptr)
    /*  determine if the symbol can be legally data initialized  */
    int sptr;
{
  switch (SCG(sptr)) {
  case SC_DUMMY:
    error(41, 3, gbl.lineno, SYMNAME(sptr), CNULL);
    goto error_exit;
  case SC_BASED:
    error(116, 3, gbl.lineno, SYMNAME(sptr), "(data initialization)");
    goto error_exit;
  case SC_CMBLK:
    if (ALLOCG(MIDNUMG(sptr))) {
      error(163, 3, gbl.lineno, SYMNAME(sptr), SYMNAME(MIDNUMG(sptr)));
      goto error_exit;
    }
    break;
  default:
    break;
  }
  if (STYPEG(sptr) == ST_ARRAY) {
    if (ALLOCG(sptr)) {
      error(84, 3, gbl.lineno, SYMNAME(sptr),
            "- initializing an allocatable array");
      goto error_exit;
    }
    if (ASUMSZG(sptr)) {
      error(84, 3, gbl.lineno, SYMNAME(sptr),
            "- initializing an assumed size array");
      goto error_exit;
    }
    if (ADJARRG(sptr)) {
      error(84, 3, gbl.lineno, SYMNAME(sptr),
            "- initializing an adjustable array");
      goto error_exit;
    }
  }

  return TRUE;

error_exit:
  sem.dinit_error = TRUE;
  return FALSE;
}

static INT
_fdiv(INT dividend, INT divisor)
{
  INT quotient;
  INT temp;

#ifdef TM_FRCP
  if (!flg.ieee) {
    xfrcp(divisor, &temp);
    xfmul(dividend, temp, &quotient);
  } else
    xfdiv(dividend, divisor, &quotient);
#else
  xfdiv(dividend, divisor, &quotient);
#endif
  return quotient;
}

static void
_ddiv(INT *dividend, INT *divisor, INT *quotient)
{
  INT temp[2];

#ifdef TM_DRCP
  if (!flg.ieee) {
    xdrcp(divisor, temp);
    xdmul(dividend, temp, quotient);
  } else
    xddiv(dividend, divisor, quotient);
#else
  xddiv(dividend, divisor, quotient);
#endif
}

static int
get_ast_op(int op)
{
  int ast_op;

  switch (op) {
  case AC_NEG:
    ast_op = OP_NEG;
    break;
  case AC_ADD:
    ast_op = OP_ADD;
    break;
  case AC_SUB:
    ast_op = OP_SUB;
    break;
  case AC_MUL:
    ast_op = OP_MUL;
    break;
  case AC_DIV:
    ast_op = OP_DIV;
    break;
  case AC_CAT:
    ast_op = OP_CAT;
    break;
  case AC_LEQV:
    ast_op = OP_LEQV;
    break;
  case AC_LNEQV:
    ast_op = OP_LNEQV;
    break;
  case AC_LOR:
    ast_op = OP_LOR;
    break;
  case AC_LAND:
    ast_op = OP_LAND;
    break;
  case AC_EQ:
    ast_op = OP_EQ;
    break;
  case AC_GE:
    ast_op = OP_GE;
    break;
  case AC_GT:
    ast_op = OP_GT;
    break;
  case AC_LE:
    ast_op = OP_LE;
    break;
  case AC_LT:
    ast_op = OP_LT;
    break;
  case AC_NE:
    ast_op = OP_NE;
    break;
  case AC_LNOT:
    ast_op = OP_LNOT;
    break;
  case AC_EXP:
    ast_op = OP_XTOI;
    break;
  case AC_EXPK:
    ast_op = OP_XTOK;
    break;
  case AC_EXPX:
    ast_op = OP_XTOX;
    break;
  default:
    interr("get_ast_op: unexpected operator in initialization expr", op, 3);
  }
  return ast_op;
}

/* Routine init_fold_const stolen from file ast.c in Fortran frontend */
static INT
init_fold_const(int opr, INT conval1, INT conval2, int dtype)
{
  IEEE128 qtemp, qresult, qnum1, qnum2;
  IEEE128 qreal1, qreal2, qrealrs, qimag1, qimag2, qimagrs;
  IEEE128 qtemp1, qtemp2;
  DBLE dtemp, dresult, num1, num2;
  DBLE dreal1, dreal2, drealrs, dimag1, dimag2, dimagrs;
  DBLE dtemp1, dtemp2;
  SNGL temp, result;
  SNGL real1, real2, realrs, imag1, imag2, imagrs;
  SNGL temp1, temp2;
  UINT val1, val2;
  INT64 inum1, inum2, ires;
  int q0;
  INT val;
  int term, sign;
  int cvlen1, cvlen2;
  char *p, *q;

  if (opr == OP_XTOI) {
    term = 1;
    if (dtype != DT_INT)
      term = cngcon(term, DT_INT, dtype);
    val = term;
    if (conval2 >= 0)
      sign = 0;
    else {
      conval2 = -conval2;
      sign = 1;
    }
    while (conval2--)
      val = init_fold_const(OP_MUL, val, conval1, dtype);
    if (sign) {
      /* exponentiation to a negative power */
      val = init_fold_const(OP_DIV, term, val, dtype);
    }
    return val;
  }
  if (opr == OP_XTOK) {
    ISZ_T cnt;
    term = stb.k1;
    if (dtype != DT_INT8)
      term = cngcon(term, DT_INT8, dtype);
    val = term;
    cnt = get_isz_cval(conval2);
    if (cnt >= 0)
      sign = 0;
    else {
      cnt = -cnt;
      sign = 1;
    }
    while (cnt--)
      val = init_fold_const(OP_MUL, val, conval1, dtype);
    if (sign) {
      /* exponentiation to a negative power */
      val = init_fold_const(OP_DIV, term, val, dtype);
    }
    return val;
  }
  switch (DTY(dtype)) {
  case TY_BINT:
  case TY_SINT:
  case TY_INT:
    switch (opr) {
    case OP_ADD:
      return conval1 + conval2;
    case OP_CMP:
      if (conval1 < conval2)
        return (INT)-1;
      if (conval1 > conval2)
        return (INT)1;
      return (INT)0;
    case OP_SUB:
      return conval1 - conval2;
    case OP_MUL:
      return conval1 * conval2;
    case OP_DIV:
      if (conval2 == 0) {
        errsev(98);
        conval2 = 1;
      }
      return conval1 / conval2;
    }
    break;

  case TY_INT8:
    inum1[0] = CONVAL1G(conval1);
    inum1[1] = CONVAL2G(conval1);
    inum2[0] = CONVAL1G(conval2);
    inum2[1] = CONVAL2G(conval2);
    switch (opr) {
    case OP_ADD:
      add64(inum1, inum2, ires);
      break;
    case OP_CMP:
      return cmp64(inum1, inum2);
    case OP_SUB:
      sub64(inum1, inum2, ires);
      break;
    case OP_MUL:
      mul64(inum1, inum2, ires);
      break;
    case OP_DIV:
      if (inum2[0] == 0 && inum2[1] == 0) {
        errsev(98);
        inum2[1] = 1;
      }
      div64(inum1, inum2, ires);
      break;
    }
    return getcon(ires, DT_INT8);

  case TY_REAL:
    switch (opr) {
    case OP_ADD:
      xfadd(conval1, conval2, &result);
      return result;
    case OP_SUB:
      xfsub(conval1, conval2, &result);
      return result;
    case OP_MUL:
      xfmul(conval1, conval2, &result);
      return result;
    case OP_DIV:
      result = _fdiv(conval1, conval2);
      return result;
    case OP_CMP:
      return xfcmp(conval1, conval2);
    case OP_XTOX:
      xfpow(conval1, conval2, &result);
      return result;
    }
    break;

  case TY_DBLE:
    num1[0] = CONVAL1G(conval1);
    num1[1] = CONVAL2G(conval1);
    num2[0] = CONVAL1G(conval2);
    num2[1] = CONVAL2G(conval2);
    switch (opr) {
    case OP_ADD:
      xdadd(num1, num2, dresult);
      break;
    case OP_SUB:
      xdsub(num1, num2, dresult);
      break;
    case OP_MUL:
      xdmul(num1, num2, dresult);
      break;
    case OP_DIV:
      _ddiv(num1, num2, dresult);
      break;
    case OP_CMP:
      return xdcmp(num1, num2);
    case OP_XTOX:
      xdpow(num1, num2, dresult);
      break;
    default:
      goto err_exit;
    }
    return getcon(dresult, DT_DBLE);

  case TY_CMPLX:
    real1 = CONVAL1G(conval1);
    imag1 = CONVAL2G(conval1);
    real2 = CONVAL1G(conval2);
    imag2 = CONVAL2G(conval2);
    switch (opr) {
    case OP_ADD:
      xfadd(real1, real2, &realrs);
      xfadd(imag1, imag2, &imagrs);
      break;
    case OP_SUB:
      xfsub(real1, real2, &realrs);
      xfsub(imag1, imag2, &imagrs);
      break;
    case OP_MUL:
      /* (a + bi) * (c + di) ==> (ac-bd) + (ad+cb)i */
      xfmul(real1, real2, &temp1);
      xfmul(imag1, imag2, &temp);
      xfsub(temp1, temp, &realrs);
      xfmul(real1, imag2, &temp1);
      xfmul(real2, imag1, &temp);
      xfadd(temp1, temp, &imagrs);
      break;
    case OP_DIV:
      /*
       *  realrs = real2;
       *  if (realrs < 0)
       *      realrs = -realrs;
       *  imagrs = imag2;
       *  if (imagrs < 0)
       *      imagrs = -imagrs;
       */
      if (xfcmp(real2, CONVAL2G(stb.flt0)) < 0)
        xfsub(CONVAL2G(stb.flt0), real2, &realrs);
      else
        realrs = real2;

      if (xfcmp(imag2, CONVAL2G(stb.flt0)) < 0)
        xfsub(CONVAL2G(stb.flt0), imag2, &imagrs);
      else
        imagrs = imag2;

      /* avoid overflow */

      if (xfcmp(realrs, imagrs) <= 0) {
        /*
         *  if (realrs <= imagrs) {
         *      temp = real2 / imag2;
         *      temp1 = 1.0f / (imag2 * (1 + temp * temp));
         *      realrs = (real1 * temp + imag1) * temp1;
         *      imagrs = (imag1 * temp - real1) * temp1;
         *  }
         */
        temp = _fdiv(real2, imag2);

        xfmul(temp, temp, &temp1);
        xfadd(CONVAL2G(stb.flt1), temp1, &temp1);
        xfmul(imag2, temp1, &temp1);
        temp1 = _fdiv(CONVAL2G(stb.flt1), temp1);

        xfmul(real1, temp, &realrs);
        xfadd(realrs, imag1, &realrs);
        xfmul(realrs, temp1, &realrs);

        xfmul(imag1, temp, &imagrs);
        xfsub(imagrs, real1, &imagrs);
        xfmul(imagrs, temp1, &imagrs);
      } else {
        /*
         *  else {
         *      temp = imag2 / real2;
         *      temp1 = 1.0f / (real2 * (1 + temp * temp));
         *      realrs = (real1 + imag1 * temp) * temp1;
         *      imagrs = (imag1 - real1 * temp) * temp1;
         *  }
         */
        temp = _fdiv(imag2, real2);

        xfmul(temp, temp, &temp1);
        xfadd(CONVAL2G(stb.flt1), temp1, &temp1);
        xfmul(real2, temp1, &temp1);
        temp1 = _fdiv(CONVAL2G(stb.flt1), temp1);

        xfmul(imag1, temp, &realrs);
        xfadd(real1, realrs, &realrs);
        xfmul(realrs, temp1, &realrs);

        xfmul(real1, temp, &imagrs);
        xfsub(imag1, imagrs, &imagrs);
        xfmul(imagrs, temp1, &imagrs);
      }
      break;
    case OP_CMP:
      /*
       * for complex, only EQ and NE comparisons are allowed, so return
       * 0 if the two constants are the same, else 1:
       */
      return (conval1 != conval2);
    default:
      goto err_exit;
    }
    num1[0] = realrs;
    num1[1] = imagrs;
    return getcon(num1, DT_CMPLX);

  case TY_DCMPLX:
    dreal1[0] = CONVAL1G(CONVAL1G(conval1));
    dreal1[1] = CONVAL2G(CONVAL1G(conval1));
    dimag1[0] = CONVAL1G(CONVAL2G(conval1));
    dimag1[1] = CONVAL2G(CONVAL2G(conval1));
    dreal2[0] = CONVAL1G(CONVAL1G(conval2));
    dreal2[1] = CONVAL2G(CONVAL1G(conval2));
    dimag2[0] = CONVAL1G(CONVAL2G(conval2));
    dimag2[1] = CONVAL2G(CONVAL2G(conval2));
    switch (opr) {
    case OP_ADD:
      xdadd(dreal1, dreal2, drealrs);
      xdadd(dimag1, dimag2, dimagrs);
      break;
    case OP_SUB:
      xdsub(dreal1, dreal2, drealrs);
      xdsub(dimag1, dimag2, dimagrs);
      break;
    case OP_MUL:
      /* (a + bi) * (c + di) ==> (ac-bd) + (ad+cb)i */
      xdmul(dreal1, dreal2, dtemp1);
      xdmul(dimag1, dimag2, dtemp);
      xdsub(dtemp1, dtemp, drealrs);
      xdmul(dreal1, dimag2, dtemp1);
      xdmul(dreal2, dimag1, dtemp);
      xdadd(dtemp1, dtemp, dimagrs);
      break;
    case OP_DIV:
      dtemp2[0] = CONVAL1G(stb.dbl0);
      dtemp2[1] = CONVAL2G(stb.dbl0);
      /*  drealrs = dreal2;
       *  if (drealrs < 0)
       *      drealrs = -drealrs;
       *  dimagrs = dimag2;
       *  if (dimagrs < 0)
       *      dimagrs = -dimagrs;
       */
      if (xdcmp(dreal2, dtemp2) < 0)
        xdsub(dtemp2, dreal2, drealrs);
      else {
        drealrs[0] = dreal2[0];
        drealrs[1] = dreal2[1];
      }
      if (xdcmp(dimag2, dtemp2) < 0)
        xdsub(dtemp2, dimag2, dimagrs);
      else {
        dimagrs[0] = dimag2[0];
        dimagrs[1] = dimag2[1];
      }

      /* avoid overflow */

      dtemp2[0] = CONVAL1G(stb.dbl1);
      dtemp2[1] = CONVAL2G(stb.dbl1);
      if (xdcmp(drealrs, dimagrs) <= 0) {
        /*  if (drealrs <= dimagrs) {
         *     dtemp = dreal2 / dimag2;
         *     dtemp1 = 1.0 / (dimag2 * (1 + dtemp * dtemp));
         *     drealrs = (dreal1 * dtemp + dimag1) * dtemp1;
         *     dimagrs = (dimag1 * dtemp - dreal1) * dtemp1;
         *  }
         */
        _ddiv(dreal2, dimag2, dtemp);

        xdmul(dtemp, dtemp, dtemp1);
        xdadd(dtemp2, dtemp1, dtemp1);
        xdmul(dimag2, dtemp1, dtemp1);
        _ddiv(dtemp2, dtemp1, dtemp1);

        xdmul(dreal1, dtemp, drealrs);
        xdadd(drealrs, dimag1, drealrs);
        xdmul(drealrs, dtemp1, drealrs);

        xdmul(dimag1, dtemp, dimagrs);
        xdsub(dimagrs, dreal1, dimagrs);
        xdmul(dimagrs, dtemp1, dimagrs);
      } else {
        /*  else {
         *  	dtemp = dimag2 / dreal2;
         *  	dtemp1 = 1.0 / (dreal2 * (1 + dtemp * dtemp));
         *  	drealrs = (dreal1 + dimag1 * dtemp) * dtemp1;
         *  	dimagrs = (dimag1 - dreal1 * dtemp) * dtemp1;
         *  }
         */
        _ddiv(dimag2, dreal2, dtemp);

        xdmul(dtemp, dtemp, dtemp1);
        xdadd(dtemp2, dtemp1, dtemp1);
        xdmul(dreal2, dtemp1, dtemp1);
        _ddiv(dtemp2, dtemp1, dtemp1);

        xdmul(dimag1, dtemp, drealrs);
        xdadd(dreal1, drealrs, drealrs);
        xdmul(drealrs, dtemp1, drealrs);

        xdmul(dreal1, dtemp, dimagrs);
        xdsub(dimag1, dimagrs, dimagrs);
        xdmul(dimagrs, dtemp1, dimagrs);
      }
      break;
    case OP_CMP:
      /*
       * for complex, only EQ and NE comparisons are allowed, so return
       * 0 if the two constants are the same, else 1:
       */
      return (conval1 != conval2);
    default:
      goto err_exit;
    }

    num1[0] = getcon(drealrs, DT_DBLE);
    num1[1] = getcon(dimagrs, DT_DBLE);
    return getcon(num1, DT_DCMPLX);

  case TY_BLOG:
  case TY_SLOG:
  case TY_LOG:
  case TY_LOG8:
    if (opr != OP_CMP) {
      goto err_exit;
    }
    /*
     * opr is assumed to be OP_CMP, only EQ and NE comparisons are
     * allowed so just return 0 if eq, else 1:
     */
    return (conval1 != conval2);
  case TY_NCHAR:
    if (opr != OP_CMP) {
      goto err_exit;
    }
#define KANJI_BLANK 0xA1A1
    {
      int bytes, val1, val2;
      /* following if condition prevent seg fault from following example;
       * logical ::b=char(32,kind=2).eq.char(45,kind=2)
       */
      if (CONVAL1G(conval1) > stb.symavl || CONVAL1G(conval2) > stb.symavl) {
        interr(
            "init_fold_const: value of kind is not supported in this context",
            dtype, 3);
        return (0);
      }

      cvlen1 = DTY(DTYPEG(CONVAL1G(conval1))) + 1;
      cvlen2 = DTY(DTYPEG(CONVAL1G(conval2))) + 1;
      p = stb.n_base + CONVAL1G(CONVAL1G(conval1));
      q = stb.n_base + CONVAL1G(CONVAL1G(conval2));

      while (cvlen1 > 0 && cvlen2 > 0) {
        val1 = kanji_char((unsigned char *)p, cvlen1, &bytes);
        p += bytes, cvlen1 -= bytes;
        val2 = kanji_char((unsigned char *)q, cvlen2, &bytes);
        q += bytes, cvlen2 -= bytes;
        if (val1 != val2)
          return (val1 - val2);
      }

      while (cvlen1 > 0) {
        val1 = kanji_char((unsigned char *)p, cvlen1, &bytes);
        p += bytes, cvlen1 -= bytes;
        if (val1 != KANJI_BLANK)
          return (val1 - KANJI_BLANK);
      }

      while (cvlen2 > 0) {
        val2 = kanji_char((unsigned char *)q, cvlen2, &bytes);
        q += bytes, cvlen2 -= bytes;
        if (val2 != KANJI_BLANK)
          return (KANJI_BLANK - val2);
      }
    }
    return 0;

  case TY_CHAR:
    if (opr != OP_CMP) {
      goto err_exit;
    }
    /* opr is OP_CMP, return -1, 0, or 1:  */
    cvlen1 = DTY((DTYPEG(conval1)) + 1);
    cvlen2 = DTY((DTYPEG(conval2)) + 1);
    if (cvlen1 == 0 || cvlen2 == 0) {
      return cvlen1 - cvlen2;
    }
    /* change the shorter string to be of same length as the longer: */
    if (cvlen1 < cvlen2) {
      conval1 = cngcon(conval1, (int)DTYPEG(conval1), (int)DTYPEG(conval2));
      cvlen1 = cvlen2;
    } else
      conval2 = cngcon(conval2, (int)DTYPEG(conval2), (int)DTYPEG(conval1));

    p = stb.n_base + CONVAL1G(conval1);
    q = stb.n_base + CONVAL1G(conval2);
    do {
      if (*p != *q)
        return (*p - *q);
      ++p;
      ++q;
    } while (--cvlen1);
    return 0;
  }

err_exit:
  interr("init_fold_const: bad args", dtype, 3);
  return (0);
}

/* Routine init_negate_const stolen from file ast.c in Fortran frontend */
static INT
init_negate_const(INT conval, int dtype)
{
  SNGL result;
  DBLE drealrs, dimagrs;
  IEEE128 qrealrs, qimagrs;
  static INT num[4], numz[4];
  int q0;

  switch (DTY(dtype)) {
  case TY_BINT:
  case TY_SINT:
  case TY_INT:
  case TY_BLOG:
  case TY_SLOG:
  case TY_LOG:
    return (-conval);

  case TY_INT8:
  case TY_LOG8:
    return init_fold_const(OP_SUB, (INT)stb.k0, conval, dtype);

  case TY_REAL:
    xfneg(conval, &result);
    return (result);

  case TY_DBLE:
    num[0] = CONVAL1G(conval);
    num[1] = CONVAL2G(conval);
    xdneg(num, drealrs);
    return getcon(drealrs, DT_DBLE);

  case TY_CMPLX:
    xfneg(CONVAL1G(conval), &num[0]); /* real part */
    xfneg(CONVAL2G(conval), &num[1]); /* imag part */
    return getcon(num, DT_CMPLX);

  case TY_DCMPLX:
    num[0] = CONVAL1G(CONVAL1G(conval));
    num[1] = CONVAL2G(CONVAL1G(conval));
    xdneg(num, drealrs);
    num[0] = CONVAL1G(CONVAL2G(conval));
    num[1] = CONVAL2G(CONVAL2G(conval));
    xdneg(num, dimagrs);
    num[0] = getcon(drealrs, DT_DBLE);
    num[1] = getcon(dimagrs, DT_DBLE);
    return getcon(num, DT_DCMPLX);

  default:
    interr("init_negate_const: bad dtype", dtype, 3);
    return (0);
  }
}

static struct {
  CONST *root;
  CONST *roottail;
  CONST *arrbase;
  int ndims;
  struct {
    int dtype;
    ISZ_T idx;
    CONST *subscr_base;
    ISZ_T lowb;
    ISZ_T upb;
    ISZ_T stride;
  } sub[7];
  struct {
    ISZ_T lowb;
    ISZ_T upb;
    ISZ_T mplyr;
  } dim[7];
} sb;

static ISZ_T
eval_sub_index(int dim)
{
  int repeatc;
  ISZ_T o_lowb, elem_offset;
  CONST *subscr_base;
  ADSC *adsc = AD_DPTR(sb.sub[dim].dtype);
  o_lowb = ad_val_of(AD_LWBD(adsc, 0));
  subscr_base = sb.sub[dim].subscr_base;

  elem_offset = (sb.sub[dim].idx - o_lowb);
  while (elem_offset && subscr_base) {
    if (subscr_base->repeatc > 1) {
      repeatc = subscr_base->repeatc;
      while (repeatc > 0 && elem_offset) {
        --repeatc;
        --elem_offset;
      }
    } else {
      subscr_base = subscr_base->next;
      --elem_offset;
    }
  }
  return get_ival(subscr_base->dtype, subscr_base->u1.conval);
}

static int
eval_sb(int d)
{
  int i;
  ISZ_T sub_idx;
  ISZ_T elem_offset;
  ISZ_T repeat;
  int t_ub = 0;
  CONST *v;
  CONST *c;
  CONST tmp;

#define TRACE_EVAL_SB 0
  if (d == 0) {
#if TRACE_EVAL_SB
    printf("-----\n");
#endif
    sb.sub[0].idx = sb.sub[0].lowb;
    /* Need to also handle negative stride of subscript triplets */
    if (sb.sub[0].stride > 0) {
      t_ub = 1;
    }
    while ((t_ub ? sb.sub[0].idx <= sb.sub[0].upb
                 : sb.sub[0].idx >= sb.sub[0].upb)) {
      /* compute element offset */
      elem_offset = 0;
      for (i = 0; i < sb.ndims; i++) {
        sub_idx = sb.sub[i].idx;
        if (sb.sub[i].subscr_base) {
          sub_idx = eval_sub_index(i);
        }
        elem_offset += (sub_idx - sb.dim[i].lowb) * sb.dim[i].mplyr;
#if TRACE_EVAL_SB
        printf("%3d ", sub_idx);
#endif
      }
#if TRACE_EVAL_SB
      printf(" elem_offset - %ld\n", elem_offset);
#endif
      /* get initialization value at element offset */
      v = sb.arrbase;
      while (v && elem_offset) {
        repeat = v->repeatc;
        if (repeat > 1) {
          while (repeat > 0 && elem_offset) {
            --elem_offset;
            --repeat;
          }
        } else {
          v = v->next;
          --elem_offset;
        }
      }
      if (v == NULL) {
        interr("initialization expression: invalid array subscripts\n",
               elem_offset, 3);
        return 1;
      }
      /*
       * evaluate initialization value and add (repeat copies) to
       * initialization list
       */
      tmp = *v;
      tmp.next = 0;
      tmp.repeatc = 1;
      c = eval_init_expr_item(clone_init_const(&tmp, TRUE));
      c->next = NULL;

      add_to_list(c, &sb.root, &sb.roottail);
      sb.sub[0].idx += sb.sub[0].stride;
    }
  loop_done:
#if TRACE_EVAL_SB
    printf("-----\n");
#endif
    return 0;
  }
  if (sb.sub[d].stride > 0) {
    for (sb.sub[d].idx = sb.sub[d].lowb; sb.sub[d].idx <= sb.sub[d].upb;
         sb.sub[d].idx += sb.sub[d].stride) {
      if (eval_sb(d - 1))
        return 1;
    }
  } else {
    for (sb.sub[d].idx = sb.sub[d].lowb; sb.sub[d].idx >= sb.sub[d].upb;
         sb.sub[d].idx += sb.sub[d].stride) {
      if (eval_sb(d - 1))
        return 1;
    }
  }
  return 0;
}

static CONST *
eval_const_array_triple_section(CONST *curr_e)
{
  int dtype;
  CONST *c, *lop, *rop;
  CONST *v;
  int ndims = 0;
  int i, abc;

  sb.root = sb.roottail = NULL;
  c = curr_e;
  do {
    rop = c->u1.expr.rop;
    lop = c->u1.expr.lop;
    sb.sub[ndims].subscr_base = 0;
    sb.sub[ndims].dtype = 0;
    /* Due to how we read in EXPR in upper.c if the lop is null the rop
     * will be put on lop instead. */
    if (rop) {
      dtype = rop->dtype;
      sb.sub[ndims].dtype = lop->dtype;
    }
    if (rop == NULL) {
      rop = lop;
      dtype = rop->dtype;
    } else if (lop) {
      CONST *t = eval_init_expr(lop);
      if (t->id == AC_ACONST)
        sb.sub[ndims].subscr_base = t->subc;
      else
        sb.sub[ndims].subscr_base = t;
    }
    /* Need to keep dtype of the original array to get actual lower/upper
     * bound when we evaluate subscript later on.
     */

    if (rop == 0) {
      interr("initialization expression: missing array section lb\n", 0, 3);
      return CONST_ERR(dtype);
    }
    v = eval_init_expr(rop);
    if (!v || v->id != AC_CONST) {
      interr("initialization expression: non-constant lb\n", 0, 3);
      return CONST_ERR(dtype);
    }
    sb.sub[ndims].lowb = get_ival(v->dtype, v->u1.conval);

    if ((rop = rop->next) == 0) {
      interr("initialization expression: missing array section ub\n", 0, 3);
      return CONST_ERR(dtype);
    }
    v = eval_init_expr(rop);
    if (!v || v->id != AC_CONST) {
      interr("initialization expression: non-constant ub\n", 0, 3);
      return CONST_ERR(dtype);
    }
    sb.sub[ndims].upb = get_ival(v->dtype, v->u1.conval);

    if ((rop = rop->next) == 0) {
      interr("initialization expression: missing array section stride\n", 0, 3);
      return CONST_ERR(dtype);
    }
    v = eval_init_expr(rop);
    if (!v || v->id != AC_CONST) {
      interr("initialization expression: non-constant stride\n", 0, 3);
      return CONST_ERR(dtype);
    }
    sb.sub[ndims].stride = get_ival(v->dtype, v->u1.conval);

    if (++ndims >= 7) {
      interr("initialization expression: too many dimensions\n", 0, 3);
      return CONST_ERR(dtype);
    }
    c = c->next;
  } while (c);

  sb.ndims = ndims;
  return sb.root;
}

static CONST *
eval_const_array_section(CONST *lop, int ldtype, int dtype)
{
  CONST *c;
  CONST *v;
  ADSC *adsc = AD_DPTR(ldtype);
  int ndims = 0;
  int i, abc;

  sb.root = sb.roottail = NULL;
  if (lop->id == AC_ACONST) {
    sb.arrbase = eval_array_constructor(lop);
  } else {
    sb.arrbase = lop;
  }

  if (sb.ndims != AD_NUMDIM(adsc)) {
    interr("initialization expression: subscript/dimension mis-match\n", ldtype,
           3);
    return CONST_ERR(dtype);
  }
  ndims = AD_NUMDIM(adsc);
  for (i = 0; i < ndims; i++) {
    sb.dim[i].lowb = ad_val_of(AD_LWBD(adsc, i));
    sb.dim[i].upb = ad_val_of(AD_UPBD(adsc, i));
    sb.dim[i].mplyr = ad_val_of(AD_MLPYR(adsc, i));
  }

  sb.ndims = ndims;
  if (eval_sb(ndims - 1))
    return CONST_ERR(dtype);

  return sb.root;
}

static CONST *
eval_ishft(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr_item(arg);
  CONST *wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  CONST *arg2 = eval_init_expr_item(arg->next);
  INT val;
  INT shftval;

  shftval = arg2->u1.conval;
  if (shftval > dtypeinfo[wrkarg->dtype].bits) {
    error(4, 3, gbl.lineno, "ISHFT SHIFT argument too big for I argument\n",
          NULL);
    return CONST_ERR(dtype);
  }

  for (; wrkarg; wrkarg = wrkarg->next) {
    if (shftval < 0) {
      wrkarg->u1.conval >>= -shftval;
    }
    if (shftval > 0) {
      wrkarg->u1.conval <<= shftval;
    }
  }

  return rslt;
}

static CONST *
eval_ichar(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr(arg);
  CONST *wrkarg;
  int srcdty;
  int rsltdtype = DDTG(dtype);
  int clen;
  int c;
  int dum;

  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  srcdty = DTY(wrkarg->dtype);
  for (; wrkarg; wrkarg = wrkarg->next) {
    if (srcdty == TY_NCHAR) {
      c = CONVAL1G(wrkarg->u1.conval);
      clen = size_of(DTYPEG(c));
      c = kanji_char((unsigned char *)stb.n_base + CONVAL1G(c), clen, &dum);
    } else {
      c = stb.n_base[CONVAL1G(wrkarg->u1.conval)] & 0xff;
    }
    wrkarg->u1.conval = cngcon(c, DT_WORD, rsltdtype);
    wrkarg->dtype = rsltdtype;
  }

  rslt->dtype = dtype;
  return rslt;
}

static CONST *
eval_char(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr_item(arg);
  CONST *wrkarg;
  int rsltdtype = DDTG(dtype);

  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (; wrkarg; wrkarg = wrkarg->next) {
    if (DT_ISWORD(wrkarg->dtype)) {
      wrkarg->u1.conval = cngcon(wrkarg->u1.conval, DT_WORD, rsltdtype);
    } else {
      wrkarg->u1.conval = cngcon(wrkarg->u1.conval, DT_DWORD, rsltdtype);
    }
    wrkarg->dtype = rsltdtype;
  }

  rslt->dtype = dtype;
  return rslt;
}

static CONST *
eval_int(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr_item(arg);
  CONST *wrkarg;
  CONST *c;
  INT result;

  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (; wrkarg; wrkarg = wrkarg->next) {
    result = cngcon(wrkarg->u1.conval, wrkarg->dtype, DDTG(dtype));

    wrkarg->id = AC_CONST;
    wrkarg->dtype = DDTG(dtype);
    wrkarg->repeatc = 1;
    wrkarg->u1.conval = result;
  }
  return rslt;
}

static CONST *
eval_null(CONST *arg, int dtype)
{
  CONST c = {0};
  CONST *p;

  p = clone_init_const(&c, TRUE);
  p->id = AC_CONST;
  p->repeatc = 1;
  p->dtype = DDTG(dtype);
  p->u1.conval = 0;
  return p;
}

static CONST *
eval_fltconvert(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr_item(arg);
  CONST *wrkarg;
  CONST *c;
  INT result;

  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (; wrkarg; wrkarg = wrkarg->next) {
    result = cngcon(wrkarg->u1.conval, wrkarg->dtype, DDTG(dtype));

    wrkarg->id = AC_CONST;
    wrkarg->dtype = DDTG(dtype);
    wrkarg->repeatc = 1;
    wrkarg->u1.conval = result;
  }
  return rslt;
}

#define GET_DBLE(x, y) \
  x[0] = CONVAL1G(y);  \
  x[1] = CONVAL2G(y)
#define GET_QUAD(x, y) \
  x[0] = CONVAL1G(y);  \
  x[1] = CONVAL2G(y);  \
  x[2] = CONVAL3G(y);  \
  x[3] = CONVAL4G(y);
#define GETVALI64(x, b) \
  x[0] = CONVAL1G(b);   \
  x[1] = CONVAL2G(b);

static CONST *
eval_abs(CONST *arg, int dtype)
{
  CONST *rslt;
  CONST *wrkarg;
  INT con1, res[4], num1[4], num2[4];
  int rsltdtype = dtype;
  double d1, d2;
  float f1, f2;

  rslt = eval_init_expr_item(arg);
  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (; wrkarg; wrkarg = wrkarg->next) {
    switch (DTY(wrkarg->dtype)) {
    case TY_SINT:
    case TY_BINT:
    case TY_INT:
      con1 = wrkarg->u1.conval;
      if (con1 < 0)
        con1 = -(con1);
      break;
    case TY_INT8:
      con1 = wrkarg->u1.conval;
      GETVALI64(num1, con1);
      GETVALI64(num2, stb.k0);
      if (cmp64(num1, num2) == -1) {
        neg64(num1, res);
        con1 = getcon(res, DT_INT8);
      }
      break;
    case TY_REAL:
      res[0] = 0;
      con1 = wrkarg->u1.conval;
      xfabsv(con1, &res[1]);
      con1 = res[1];
      break;
    case TY_DBLE:
      con1 = wrkarg->u1.conval;
      GET_DBLE(num1, con1);
      xdabsv(num1, res);
      con1 = getcon(res, dtype);
      break;
    case TY_CMPLX:
      con1 = wrkarg->u1.conval;
      num1[0] = CONVAL1G(con1);
      num1[1] = CONVAL2G(con1);
      xfmul(num1[0], num1[0], &num2[0]);
      xfmul(num1[1], num1[1], &num2[1]);
      xfadd(num2[0], num2[1], &num2[2]);
      xfsqrt(num2[2], &con1);
      wrkarg->dtype = DT_REAL;
      dtype = rsltdtype = DT_REAL;
      break;
    case TY_DCMPLX:
      con1 = wrkarg->u1.conval;
      wrkarg->dtype = DT_DBLE;
      dtype = rsltdtype = DT_DBLE;

      break;
    default:
      con1 = wrkarg->u1.conval;
      break;
    }

    wrkarg->u1.conval = cngcon(con1, wrkarg->dtype, rsltdtype);
    wrkarg->dtype = dtype;
  }
  return rslt;
}

static CONST *
eval_min(CONST *arg, int dtype)
{
  CONST **arglist;
  CONST *rslt;
  CONST *wrkarg1;
  CONST *wrkarg2;
  CONST *c;
  int conval;
  int nargs;
  int nelems = 1;
  int i, j, k;

  if (DTY(arg->dtype) == TY_ARRAY) {
    nelems = ad_val_of(AD_NUMELM(AD_DPTR(arg->dtype)));
  }

  for (wrkarg1 = arg, nargs = 0; wrkarg1; wrkarg1 = wrkarg1->next, nargs++)
    ;
  NEW(arglist, CONST *, nargs);
  for (i = 0, wrkarg1 = arg; i < nargs; i++, wrkarg1 = wrkarg1->next) {
    arglist[i] = eval_init_expr_item(wrkarg1);
  }

  rslt = clone_init_const_list(arglist[0], TRUE);
  wrkarg1 = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (i = 0; i < nelems; i++) {

    for (j = 1; j < nargs; j++) {
      wrkarg2 = arglist[j]->id == AC_ACONST ? arglist[j]->subc : arglist[j];

      for (k = 0; k < i; k++) {
        wrkarg2 = wrkarg2->next;
      }
      switch (DTY(dtype)) {
      case TY_INT:
        if (wrkarg2->u1.conval < wrkarg1->u1.conval) {
          wrkarg1->u1 = wrkarg2->u1;
        }
        break;
      case TY_CHAR:
        if (strcmp(stb.n_base + CONVAL1G(wrkarg2->u1.conval),
                   stb.n_base + CONVAL1G(wrkarg1->u1.conval)) < 0) {
          wrkarg1->u1 = wrkarg2->u1;
        }
        break;
      case TY_REAL:
        if (xfcmp(wrkarg2->u1.conval, wrkarg1->u1.conval) < 0) {
          wrkarg1->u1 = wrkarg2->u1;
        }
        break;
      case TY_INT8:
      case TY_DBLE:
        if (init_fold_const(OP_CMP, wrkarg2->u1.conval, wrkarg1->u1.conval,
                            dtype) < 0) {
          wrkarg1->u1 = wrkarg2->u1;
        }
        break;
      }
    }
    wrkarg1 = wrkarg1->next;
  }
  FREE(arglist);

  return rslt;
}

static CONST *
eval_max(CONST *arg, int dtype)
{

  CONST **arglist;
  CONST *rslt;
  CONST *wrkarg1;
  CONST *wrkarg2;
  CONST *c;
  int conval;
  int nargs;
  int nelems = 1;
  int i, j, k;

  if (DTY(arg->dtype) == TY_ARRAY) {
    nelems = ad_val_of(AD_NUMELM(AD_DPTR(arg->dtype)));
  }

  for (wrkarg1 = arg, nargs = 0; wrkarg1; wrkarg1 = wrkarg1->next, nargs++)
    ;
  NEW(arglist, CONST *, nargs);
  for (i = 0, wrkarg1 = arg; i < nargs; i++, wrkarg1 = wrkarg1->next) {
    arglist[i] = eval_init_expr_item(wrkarg1);
  }

  rslt = clone_init_const_list(arglist[0], TRUE);
  wrkarg1 = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (i = 0; i < nelems; i++) {

    for (j = 1; j < nargs; j++) {
      wrkarg2 = arglist[j]->id == AC_ACONST ? arglist[j]->subc : arglist[j];

      for (k = 0; k < i; k++) {
        wrkarg2 = wrkarg2->next;
      }
      switch (DTY(dtype)) {
      case TY_CHAR:
        if (strcmp(stb.n_base + CONVAL1G(wrkarg2->u1.conval),
                   stb.n_base + CONVAL1G(wrkarg1->u1.conval)) > 0) {
          wrkarg1->u1 = wrkarg2->u1;
        }
        break;
      case TY_INT:
        if (wrkarg2->u1.conval > wrkarg1->u1.conval) {
          wrkarg1->u1 = wrkarg2->u1;
        }
        break;
      case TY_REAL:
        if (xfcmp(wrkarg2->u1.conval, wrkarg1->u1.conval) > 0) {
          wrkarg1->u1 = wrkarg2->u1;
        }

        break;
      case TY_INT8:
      case TY_DBLE:
        if (init_fold_const(OP_CMP, wrkarg2->u1.conval, wrkarg1->u1.conval,
                            dtype) > 0) {
          wrkarg1->u1 = wrkarg2->u1;
        }
        break;
      }
    }
    wrkarg1 = wrkarg1->next;
  }
  FREE(arglist);

  return rslt;
}

static CONST *
eval_nint(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr_item(arg);
  CONST *wrkarg;
  CONST *c;
  int conval;

  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (; wrkarg; wrkarg = wrkarg->next) {
    INT num1[4];
    INT res[4];
    INT con1;
    int dtype1 = wrkarg->dtype;

    con1 = wrkarg->u1.conval;
    switch (DTY(dtype1)) {
    case TY_REAL:
      num1[0] = CONVAL2G(stb.flt0);
      if (xfcmp(con1, num1[0]) >= 0)
        xfadd(con1, CONVAL2G(stb.flthalf), &res[0]);
      else
        xfsub(con1, CONVAL2G(stb.flthalf), &res[0]);
      conval = cngcon(res[0], DT_REAL, DT_INT);
      break;
    case TY_DBLE:
      if (init_fold_const(OP_CMP, con1, stb.dbl0, DT_DBLE) >= 0)
        res[0] = init_fold_const(OP_ADD, con1, stb.dblhalf, DT_DBLE);
      else
        res[0] = init_fold_const(OP_SUB, con1, stb.dblhalf, DT_DBLE);
      conval = cngcon(res[0], DT_DBLE, DT_INT);
      break;
    }

    wrkarg->id = AC_CONST;
    wrkarg->dtype = DT_INT;
    wrkarg->repeatc = 1;
    wrkarg->u1.conval = conval;
  }
  return rslt;
}

static CONST *
eval_floor(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr_item(arg);
  CONST *wrkarg;
  int conval;

  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (; wrkarg; wrkarg = wrkarg->next) {
    INT num1[4];
    INT con1;
    int adjust;

    adjust = 0;
    con1 = wrkarg->u1.conval;
    switch (DTY(wrkarg->dtype)) {
    case TY_REAL:
      conval = cngcon(con1, DT_REAL, dtype);
      num1[0] = CONVAL2G(stb.flt0);
      if (xfcmp(con1, num1[0]) < 0) {
        con1 = cngcon(conval, dtype, DT_REAL);
        if (xfcmp(con1, wrkarg->u1.conval) != 0)
          adjust = 1;
      }
      break;
    case TY_DBLE:
      conval = cngcon(con1, DT_DBLE, dtype);
      if (init_fold_const(OP_CMP, con1, stb.dbl0, DT_DBLE) < 0) {
        con1 = cngcon(conval, dtype, DT_DBLE);
        if (init_fold_const(OP_CMP, con1, wrkarg->u1.conval, DT_DBLE) != 0)
          adjust = 1;
      }
      break;
    }
    if (adjust) {
      if (DT_ISWORD(dtype))
        conval--;
      else {
        num1[0] = 0;
        num1[1] = 1;
        con1 = getcon(num1, dtype);
        conval = init_fold_const(OP_SUB, conval, con1, dtype);
      }
    }
    wrkarg->u1.conval = conval;
    wrkarg->dtype = dtype;
    wrkarg->id = AC_CONST;
    wrkarg->repeatc = 1;
  }
  return rslt;
}

static CONST *
eval_ceiling(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr_item(arg);
  CONST *wrkarg;
  int conval;

  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (; wrkarg; wrkarg = wrkarg->next) {
    INT num1[4];
    INT con1;
    int adjust;

    adjust = 0;
    con1 = wrkarg->u1.conval;
    switch (DTY(wrkarg->dtype)) {
    case TY_REAL:
      conval = cngcon(con1, DT_REAL, dtype);
      num1[0] = CONVAL2G(stb.flt0);
      if (xfcmp(con1, num1[0]) > 0) {
        con1 = cngcon(conval, dtype, DT_REAL);
        if (xfcmp(con1, wrkarg->u1.conval) != 0)
          adjust = 1;
      }
      break;
    case TY_DBLE:
      conval = cngcon(con1, DT_DBLE, dtype);
      if (init_fold_const(OP_CMP, con1, stb.dbl0, DT_DBLE) > 0) {
        con1 = cngcon(conval, dtype, DT_DBLE);
        if (init_fold_const(OP_CMP, con1, wrkarg->u1.conval, DT_DBLE) != 0)
          adjust = 1;
      }
      break;
    }
    if (adjust) {
      if (DT_ISWORD(dtype))
        conval++;
      else {
        num1[0] = 0;
        num1[1] = 1;
        con1 = getcon(num1, dtype);
        conval = init_fold_const(OP_ADD, conval, con1, dtype);
      }
    }
    wrkarg->u1.conval = conval;
    wrkarg->dtype = dtype;
    wrkarg->id = AC_CONST;
    wrkarg->repeatc = 1;
  }
  return rslt;
}

static CONST *
eval_mod(CONST *arg, int dtype)
{
  CONST *rslt;
  CONST *arg1, *arg2;
  INT conval;
  arg1 = eval_init_expr_item(arg);
  arg2 = eval_init_expr_item(arg->next);
  rslt = clone_init_const_list(arg1, TRUE);
  arg1 = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  arg2 = (arg2->id == AC_ACONST ? arg2->subc : arg2);
  for (; arg1; arg1 = arg1->next, arg2 = arg2->next) {
    /* mod(a,p) == a-int(a/p)*p  */
    INT num1[4], num2[4], num3[4];
    INT con1, con2, con3;
    con1 = arg1->u1.conval;
    con2 = arg2->u1.conval;
    /*
            conval1 = cngcon(arg1->u1.conval, arg1->dtype, dtype);
            conval2 = cngcon(arg2->u1.conval, arg2->dtype, dtype);
            conval3 = const_fold(OP_DIV, conval1, conval2, dtype);
            conval3 = cngcon(conval3, dtype, DT_INT8);
            conval3 = cngcon(conval3, DT_INT8, dtype);
            conval3 = const_fold(OP_MUL, conval3, conval2, dtype);
            conval3 = const_fold(OP_SUB, conval1, conval3, dtype);
            arg1->conval = conval3;
     */
    switch (DTY(arg1->dtype)) {
    case TY_REAL:
      xfdiv(con1, con2, &con3);
      con3 = cngcon(con3, DT_REAL, DT_INT8);
      con3 = cngcon(con3, DT_INT8, DT_REAL);
      xfmul(con3, con2, &con3);
      xfsub(con1, con3, &con3);
      conval = con3;
      break;
    case TY_DBLE:
      num1[0] = CONVAL1G(con1);
      num1[1] = CONVAL2G(con1);
      num2[0] = CONVAL1G(con2);
      num2[1] = CONVAL2G(con2);
      xddiv(num1, num2, num3);
      con3 = getcon(num3, DT_DBLE);
      con3 = cngcon(con3, DT_DBLE, DT_INT8);
      con3 = cngcon(con3, DT_INT8, DT_DBLE);
      num3[0] = CONVAL1G(con3);
      num3[1] = CONVAL2G(con3);
      xdmul(num3, num2, num3);
      xdsub(num1, num3, num3);
      conval = getcon(num3, DT_DBLE);
      break;
    case TY_CMPLX:
    case TY_DCMPLX:
      error(155, 3, gbl.lineno, "Intrinsic not supported in initialization:",
            "mod");
      break;
    default:
      error(155, 3, gbl.lineno, "Intrinsic not supported in initialization:",
            "mod");
      break;
    }
    conval = cngcon(conval, arg1->dtype, dtype);
    arg1->u1.conval = conval;
    arg1->dtype = dtype;
    arg1->id = AC_CONST;
    arg1->repeatc = 1;
  }
  return rslt;
}

static CONST *
eval_repeat(CONST *arg, int dtype)
{
  CONST *rslt = NULL;
  CONST *c;
  CONST *arg1 = eval_init_expr_item(arg);
  CONST *arg2 = eval_init_expr_item(arg->next);
  int i, j, cvlen, newlen, result;
  int ncopies;
  char *p, *cp, *str;
  char ch;

  ncopies = arg2->u1.conval;
  newlen = size_of(dtype);
  cvlen = size_of(arg1->dtype);

  str = cp = getitem(0, newlen);
  j = ncopies;
  while (j-- > 0) {
    p = stb.n_base + CONVAL1G(arg1->u1.conval);
    i = cvlen;
    while (i-- > 0)
      *cp++ = *p++;
  }
  result = getstring(str, newlen);

  rslt = (CONST *)getitem(4, sizeof(CONST));
  BZERO(rslt, CONST, 1);
  rslt->id = AC_CONST;
  rslt->dtype = dtype;
  rslt->repeatc = 1;
  rslt->u1.conval = result;

  return rslt;
}

static CONST *
eval_len_trim(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr_item(arg);
  CONST *wrkarg;
  char *p;
  int i, cvlen, result;

  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (; wrkarg; wrkarg = wrkarg->next) {
    p = stb.n_base + CONVAL1G(wrkarg->u1.conval);
    result = cvlen = size_of(wrkarg->dtype);
    i = 0;
    p += cvlen - 1;
    /* skip trailing blanks */
    while (cvlen-- > 0) {
      if (*p-- != ' ')
        break;
      result--;
    }

    wrkarg->id = AC_CONST;
    wrkarg->dtype = DT_INT;
    wrkarg->repeatc = 1;
    wrkarg->u1.conval = result;
  }
  return rslt;
}

static CONST *
eval_selected_real_kind(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr_item(arg);
  CONST *wrkarg;
  int r;
  int con;

  r = 4;

  wrkarg = eval_init_expr_item(arg);
  con = wrkarg->u1.conval; /* what about zero ?? */
  if (con <= 6)
    r = 4;
  else if (con <= 15)
    r = 8;
  else
    r = -1;

  if (arg->next) {
    wrkarg = eval_init_expr_item(arg->next);
    con = wrkarg->u1.conval; /* what about zero ?? */
    if (con <= 37) {
      if (r > 0 && r < 4)
        r = 4;
    } else if (con <= 307) {
      if (r > 0 && r < 8)
        r = 8;
    } else {
      if (r > 0)
        r = 0;
      r -= 2;
    }
  }

  rslt = (CONST *)getitem(4, sizeof(CONST));
  BZERO(rslt, CONST, 1);
  rslt->id = AC_CONST;
  rslt->dtype = DT_INT;
  rslt->repeatc = 1;
  rslt->u1.conval = r;

  return rslt;
}

static CONST *
eval_selected_int_kind(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr_item(arg);
  CONST *wrkarg;
  int r;
  int con;

  wrkarg = eval_init_expr_item(arg);
  con = wrkarg->u1.conval;
  if (con > 18 || (con > 9 && XBIT(57, 2)))
    r = -1;
  else if (con > 9)
    r = 8;
  else if (con > 4)
    r = 4;
  else if (con > 2)
    r = 2;
  else
    r = 1;
  rslt->u1.conval = r;

  return rslt;
}

extern LOGICAL sem_eq_str(int, char *); /* semutil0.c */

/** \brief Check charset
 *
 * Note: make sure this routine is consistent with
 * - fe90:        semfunc.c:_selected_char_kind()
 * - runtime/f90: miscsup_com.c:_selected_char_kind()
 */
static int
_selected_char_kind(int con)
{
  if (sem_eq_str(con, "ASCII"))
    return 1;
  else if (sem_eq_str(con, "DEFAULT"))
    return 1;
  return -1;
}

static CONST *
eval_selected_char_kind(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr(arg);
  int r;
  int con;

  con = rslt->u1.conval;
  if (sem_eq_str(con, "ASCII"))
    r = 1;
  else if (sem_eq_str(con, "DEFAULT"))
    return (CONST *)1;
  else
    r = -1;
  rslt = (CONST *)getitem(4, sizeof(CONST));
  BZERO(rslt, CONST, 1);
  rslt->id = AC_CONST;
  rslt->dtype = DT_INT;
  rslt->repeatc = 1;
  rslt->u1.conval = r;
  return rslt;
}

static CONST *
eval_scan(CONST *arg, int dtype)
{
  CONST *rslt = NULL;
  CONST *rslttail = NULL;
  CONST *c;
  CONST *wrkarg;
  int i, j;
  int l_string, l_set;
  char *p_string, *p_set;
  ISZ_T back = 0;

  assert(arg->next, "eval_scan: substring argument missing\n", 0, 4);
  wrkarg = eval_init_expr_item(arg->next);
  p_set = stb.n_base + CONVAL1G(wrkarg->u1.conval);
  l_set = size_of(wrkarg->dtype);

  if (arg->next->next) {
    wrkarg = eval_init_expr_item(arg->next->next);
    back = get_ival(wrkarg->dtype, wrkarg->u1.conval);
  }

  wrkarg = (arg->id == AC_ACONST ? arg->subc : arg);
  wrkarg = eval_init_expr_item(wrkarg);
  for (; wrkarg; wrkarg = wrkarg->next) {
    assert(wrkarg->id == AC_CONST, "eval_scan: non-constant argument\n", 0, 4);
    p_string = stb.n_base + CONVAL1G(wrkarg->u1.conval);
    l_string = size_of(wrkarg->dtype);

    c = (CONST *)getitem(4, sizeof(CONST));
    BZERO(c, CONST, 1);
    c->id = AC_CONST;
    c->dtype = DT_INT;
    c->repeatc = 1;

    if (back == 0) {
      for (i = 0; i < l_string; ++i)
        for (j = 0; j < l_set; ++j)
          if (p_set[j] == p_string[i]) {
            c->u1.conval = i + 1;
            goto addtolist;
          }
    } else {
      for (i = l_string - 1; i >= 0; --i)
        for (j = 0; j < l_set; ++j)
          if (p_set[j] == p_string[i]) {
            c->u1.conval = i + 1;
            goto addtolist;
          }
    }
    c->u1.conval = 0;

  addtolist:
    add_to_list(c, &rslt, &rslttail);
  }
  return rslt;
}

static CONST *
eval_verify(CONST *arg, int dtype)
{
  CONST *rslt = NULL;
  CONST *rslttail = NULL;
  CONST *c;
  CONST *wrkarg;
  int i, j;
  int l_string, l_set;
  char *p_string, *p_set;
  ISZ_T back = 0;

  assert(arg->next, "eval_verify: substring argument missing\n", 0, 4);
  wrkarg = eval_init_expr_item(arg->next);
  p_set = stb.n_base + CONVAL1G(wrkarg->u1.conval);
  l_set = size_of(wrkarg->dtype);

  if (arg->next->next) {
    wrkarg = eval_init_expr_item(arg->next->next);
    back = get_ival(wrkarg->dtype, wrkarg->u1.conval);
  }

  wrkarg = (arg->id == AC_ACONST ? arg->subc : arg);
  wrkarg = eval_init_expr_item(wrkarg);
  for (; wrkarg; wrkarg = wrkarg->next) {
    assert(wrkarg->id == AC_CONST, "eval_verify: non-constant argument\n", 0,
           4);
    p_string = stb.n_base + CONVAL1G(wrkarg->u1.conval);
    l_string = size_of(wrkarg->dtype);

    c = (CONST *)getitem(4, sizeof(CONST));
    BZERO(c, CONST, 1);
    c->id = AC_CONST;
    c->dtype = DT_INT;
    c->repeatc = 1;
    c->u1.conval = 0;

    if (back == 0) {
      for (i = 0; i < l_string; ++i) {
        for (j = 0; j < l_set; ++j) {
          if (p_set[j] == p_string[i])
            goto contf;
        }
        c->u1.conval = i + 1;
        break;
      contf:;
      }
    } else {
      for (i = l_string - 1; i >= 0; --i) {
        for (j = 0; j < l_set; ++j) {
          if (p_set[j] == p_string[i])
            goto contb;
        }
        c->u1.conval = i + 1;
        break;
      contb:;
      }
    }

  addtolist:
    add_to_list(c, &rslt, &rslttail);
  }
  return rslt;
}

static CONST *
eval_index(CONST *arg, int dtype)
{
  CONST *rslt = NULL;
  CONST *rslttail = NULL;
  CONST *c;
  CONST *wrkarg;
  int i, n;
  int l_string, l_substring;
  char *p_string, *p_substring;
  ISZ_T back = 0;

  assert(arg->next, "eval_index: substring argument missing\n", 0, 4);
  wrkarg = eval_init_expr_item(arg->next);
  p_substring = stb.n_base + CONVAL1G(wrkarg->u1.conval);
  l_substring = size_of(wrkarg->dtype);

  if (arg->next->next) {
    wrkarg = eval_init_expr_item(arg->next->next);
    back = get_ival(wrkarg->dtype, wrkarg->u1.conval);
  }

  wrkarg = (arg->id == AC_ACONST ? arg->subc : arg);
  wrkarg = eval_init_expr_item(wrkarg);
  for (; wrkarg; wrkarg = wrkarg->next) {
    assert(wrkarg->id == AC_CONST, "eval_index: non-constant argument\n", 0, 4);
    p_string = stb.n_base + CONVAL1G(wrkarg->u1.conval);
    l_string = size_of(wrkarg->dtype);

    c = (CONST *)getitem(4, sizeof(CONST));
    BZERO(c, CONST, 1);
    c->id = AC_CONST;
    c->dtype = DT_INT;
    c->repeatc = 1;

    n = l_string - l_substring;
    if (n < 0)
      c->u1.conval = 0;
    if (back == 0) {
      if (l_substring == 0)
        c->u1.conval = 1;
      for (i = 0; i <= n; ++i) {
        if (p_string[i] == p_substring[0] &&
            strncmp(p_string + i, p_substring, l_substring) == 0)
          c->u1.conval = i + 1;
      }
    } else {
      if (l_substring == 0)
        c->u1.conval = l_string + 1;
      for (i = n; i >= 0; --i) {
        if (p_string[i] == p_substring[0] &&
            strncmp(p_string + i, p_substring, l_substring) == 0)
          c->u1.conval = i + 1;
      }
    }
    add_to_list(c, &rslt, &rslttail);
  }
  return rslt;
}

static CONST *
eval_trim(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr(arg);
  CONST *wrkarg;
  char *p, *cp, *str;
  int i, cvlen, newlen, result;

  p = stb.n_base + CONVAL1G(rslt->u1.conval);
  cvlen = newlen = size_of(rslt->dtype);

  i = 0;
  p += cvlen - 1;
  /* skip trailing blanks */
  while (cvlen-- > 0) {
    if (*p-- != ' ')
      break;
    newlen--;
  }

  if (newlen == 0) {
    str = " ";
    rslt->u1.conval = getstring(str, strlen(str));
  } else {
    str = cp = getitem(0, newlen);
    i = newlen;
    cp += newlen - 1;
    p++;
    while (i-- > 0) {
      *cp-- = *p--;
    }
    rslt->u1.conval = getstring(str, newlen);
  }

  rslt->dtype = get_type(2, DTY(dtype), newlen);
  return rslt;
}

static CONST *
eval_adjustl(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr(arg);
  CONST *wrkarg;
  char *p, *cp, *str;
  char ch;
  int i, cvlen, origlen, result;
  INT val[2];

  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (; wrkarg; wrkarg = wrkarg->next) {
    assert(wrkarg->id == AC_CONST, "eval_adjustl: non-constant argument\n", 0,
           4);
    p = stb.n_base + CONVAL1G(wrkarg->u1.conval);
    cvlen = size_of(wrkarg->dtype);
    origlen = cvlen;
    str = cp = getitem(0, cvlen + 1); /* +1 just in case cvlen is 0 */
    i = 0;
    /* left justify string - skip leading blanks */
    while (cvlen-- > 0) {
      ch = *p++;
      if (ch != ' ') {
        *cp++ = ch;
        break;
      }
      i++;
    }
    while (cvlen-- > 0)
      *cp++ = *p++;
    /* append blanks */
    while (i-- > 0)
      *cp++ = ' ';
    wrkarg->u1.conval = getstring(str, origlen);
  }

  return rslt;
}

static CONST *
eval_adjustr(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr(arg);
  CONST *wrkarg;
  char *p, *cp, *str;
  char ch;
  int i, cvlen, origlen, result;
  INT val[2];

  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (; wrkarg; wrkarg = wrkarg->next) {
    assert(wrkarg->id == AC_CONST, "eval_adjustl: non-constant argument\n", 0,
           4);
    p = stb.n_base + CONVAL1G(wrkarg->u1.conval);
    origlen = cvlen = size_of(wrkarg->dtype);
    str = cp = getitem(0, cvlen + 1); /* +1 just in case cvlen is 0 */
    i = 0;
    p += cvlen - 1;
    cp += cvlen - 1;
    /* right justify string - skip trailing blanks */
    while (cvlen-- > 0) {
      ch = *p--;
      if (ch != ' ') {
        *cp-- = ch;
        break;
      }
      i++;
    }
    while (cvlen-- > 0)
      *cp-- = *p--;
    /* insert blanks */
    while (i-- > 0)
      *cp-- = ' ';
    wrkarg->u1.conval = getstring(str, origlen);
  }

  return rslt;
}

static CONST *
eval_shape(CONST *arg, int dtype)
{
  CONST *rslt;

  rslt = clone_init_const(arg, TRUE);
  return rslt;
}

static CONST *
eval_size(CONST *arg, int dtype)
{
  CONST *arg1 = arg;
  CONST *arg2 = arg->next;
  CONST *arg3;
  CONST *rslt;
  int dim;
  int i;

  if ((arg3 = arg->next->next)) {
    arg3 = eval_init_expr_item(arg3);
    dim = arg3->u1.conval;
    arg2 = arg2->subc;
    for (i = 1; i < dim && arg2; i++) {
      arg2 = arg2->next;
    }
    rslt = clone_init_const(arg2, TRUE);
  } else {
    rslt = clone_init_const(arg1, TRUE);
  }

  return rslt;
}

static CONST *
eval_ul_bound(int ul_selector, CONST *arg, int dtype)
{
  CONST *arg1 = arg;
  CONST *arg2;
  int arg2const;
  CONST *rslt;
  ADSC *adsc = AD_DPTR(arg1->dtype);
  int rank = AD_UPBD(adsc, 0);
  int i;

  if (arg->next) {
    arg2 = eval_init_expr_item(arg->next);
    arg2const = arg2->u1.conval;
    if (arg2const > rank) {
      error(155, 3, gbl.lineno, "DIM argument greater than the array rank",
            CNULL);
      return CONST_ERR(dtype);
    }
    rslt = arg1->subc;
    for (i = 1; rslt && i < arg2const; i++) {
      rslt = rslt->next;
    }
    rslt = clone_init_const(rslt, TRUE);
  } else {
    rslt = clone_init_const(arg1, TRUE);
  }
  return rslt;
}

static int
copy_initconst_to_array(CONST **arr, CONST *c, int count)
{
  int i;
  int acnt;
  CONST *acl, **t;
  t = arr;

  for (i = 0; i < count;) {
    if (c == NULL)
      break;
    switch (c->id) {
    case AC_ACONST:
      acnt = copy_initconst_to_array(arr, c->subc,
                                     count - i); /* MORE: count - i??? */
      i += acnt;
      arr += acnt;
      break;
    case AC_CONST:
      acl = *arr = clone_init_const(c, TRUE);
      if (acl->repeatc > 1) {
        arr += acl->repeatc;
        i += acl->repeatc;
      } else {
        arr++;
        i++;
      }
      break;
    default:
      interr("copy_initconst_to_array: unexpected const type", c->id, 3);
      return count;
    }
    c = c->next;
  }
  return i;
}

static CONST *
eval_reshape(CONST *arg, int dtype)
{
  CONST *srclist = eval_init_expr_item(arg);
  CONST *srci, *tacl;
  CONST *shape = eval_init_expr_item(arg->next);
  CONST *pad = NULL;
  CONST *wrklist = NULL;
  CONST *orderarg = NULL;
  CONST **old_val = NULL;
  CONST **new_val = NULL;
  CONST *c = NULL;
  ADSC *adsc = AD_DPTR(dtype);
  int *new_index;
  int src_sz, dest_sz;
  int rank;
  int order[7];
  int lwb[7];
  int upb[7];
  int mult[7];
  int i;
  int count;
  int sz;

  if (arg->next->next) {
    pad = arg->next->next;
    if (pad->id != AC_CONST) {
      pad = eval_init_expr_item(pad);
    }
    if (arg->next->next->next && arg->next->next->next->id != AC_CONST) {
      orderarg = eval_init_expr_item(arg->next->next->next);
    }
  }
  src_sz = ad_val_of(AD_NUMELM(AD_DPTR(arg->dtype)));
  dest_sz = ad_val_of(AD_NUMELM(adsc));

  rank = AD_NUMDIM(adsc);
  sz = 1;
  for (i = 0; i < rank; i++) {
    upb[i] = ad_val_of(AD_UPBD(adsc, i));
    lwb[i] = 0;
    mult[i] = sz;
    sz *= upb[i];
  }

  if (orderarg == NULL) {
    if (src_sz == dest_sz) {
      return srclist;
    }
    for (i = 0; i < rank; i++) {
      order[i] = i;
    }
  } else {
    LOGICAL out_of_order;

    out_of_order = FALSE;
    c = (orderarg->id == AC_ACONST ? orderarg->subc : orderarg);
    for (i = 0; c && i < rank; c = c->next, i++) {
      order[i] =
          DT_ISWORD(c->dtype) ? c->u1.conval - 1 : ad_val_of(c->u1.conval) - 1;
      if (order[i] != i)
        out_of_order = TRUE;
    }
    if (!out_of_order && src_sz == dest_sz) {
      return srclist;
    }
  }

  NEW(old_val, CONST *, dest_sz);
  if (old_val == NULL)
    return CONST_ERR(dtype);
  BZERO(old_val, CONST *, dest_sz);
  NEW(new_val, CONST *, dest_sz);
  if (new_val == NULL) {
    return CONST_ERR(dtype);
  }
  BZERO(new_val, CONST *, dest_sz);
  NEW(new_index, int, dest_sz);
  if (new_index == NULL) {
    return CONST_ERR(dtype);
  }
  BZERO(new_index, int, dest_sz);

  count = dest_sz > src_sz ? src_sz : dest_sz;
  wrklist = srclist->id == AC_ACONST ? srclist->subc : srclist;
  (void)copy_initconst_to_array(old_val, wrklist, count);

  if (dest_sz > src_sz) {
    count = dest_sz - src_sz;
    wrklist = pad->id == AC_ACONST ? pad->subc : pad;
    while (count > 0) {
      i = copy_initconst_to_array(old_val + src_sz, wrklist, count);
      count -= i;
      src_sz += i;
    }
  }

  /* index to access source in linear order */
  i = 0;
  while (TRUE) {
    int index; /* index where to store each element of new val */
    int j;

    index = 0;
    for (j = 0; j < rank; j++)
      index += lwb[j] * mult[j];

    new_index[index] = i;

    /* update loop indices */
    for (j = 0; j < rank; j++) {
      int loop;
      loop = order[j];
      lwb[loop]++;
      if (lwb[loop] < upb[loop])
        break;
      lwb[loop] = 0; /* reset and go on to the next loop */
    }
    if (j >= rank)
      break;
    i++;
  }

  for (i = 0; i < dest_sz; i++) {
    CONST *tail;
    int idx, start, end;
    int index = new_index[i];
    if (old_val[index]) {
      if (old_val[index]->repeatc <= 1) {
        new_val[i] = old_val[index];
        new_val[i]->id = AC_CONVAL;
      } else {
        idx = index + 1;
        start = i;
        end = old_val[index]->repeatc - 1;
        while (new_index[++start] == idx) {
          ++idx;
          --end;
          if (end <= 0 || start > dest_sz - 1)
            break;
        }
        old_val[index]->next = NULL;
        tacl = clone_init_const(old_val[index], TRUE);
        tacl->repeatc = idx - index;
        tacl->id = AC_CONVAL;
        old_val[index]->repeatc = index - (idx - index);
        new_val[i] = tacl;
      }
    } else {
      tail = old_val[index];
      idx = index;
      while (tail == NULL && idx >= 0) {
        tail = old_val[idx--];
      }
      tail->next = NULL;
      tacl = clone_init_const(tail, TRUE);
      start = i;
      end = tail->repeatc - 1;
      idx = index + 1;
      while (new_index[++start] == idx) {
        ++idx;
        --end;
        if (end <= 0 || start > dest_sz - 1)
          break;
      }
      tail->repeatc = index - (idx - index);
      tacl->repeatc = idx - index;
      tacl->id = AC_CONVAL;
      new_val[i] = tacl;
    }
  }
  tacl = new_val[0];
  for (i = 0; i < dest_sz - 1; ++i) {
    if (new_val[i + 1] == NULL) {
      continue;
    } else {
      tacl->next = new_val[i + 1];
      tacl = new_val[i + 1];
    }
  }
  if (new_val[dest_sz - 1])
    (new_val[dest_sz - 1])->next = NULL;
  srclist = *new_val;

  FREE(old_val);
  FREE(new_val);
  FREE(new_index);

  return srclist;
}

/* Store the value 'conval' of type 'dtype' into 'destination'. */
static void
transfer_store(INT conval, int dtype, char *destination)
{
  int *dest = (int *)destination;
  INT real, imag;

  if (DT_ISWORD(dtype)) {
    dest[0] = conval;
    return;
  }

  switch (DTY(dtype)) {
  case TY_DWORD:
  case TY_INT8:
  case TY_LOG8:
  case TY_DBLE:
    dest[0] = CONVAL2G(conval);
    dest[1] = CONVAL1G(conval);
    break;

  case TY_CMPLX:
    dest[0] = CONVAL1G(conval);
    dest[1] = CONVAL2G(conval);
    break;

  case TY_DCMPLX:
    real = CONVAL1G(conval);
    imag = CONVAL2G(conval);
    dest[0] = CONVAL2G(real);
    dest[1] = CONVAL1G(real);
    dest[2] = CONVAL2G(imag);
    dest[3] = CONVAL1G(imag);
    break;

  case TY_CHAR:
    memcpy(dest, stb.n_base + CONVAL1G(conval), size_of(dtype));
    break;

  default:
    interr("transfer_store: unexpected dtype", dtype, 3);
  }
}

/* Get a value of type 'dtype' from buffer 'source'. */
static INT
transfer_load(int dtype, char *source)
{
  int *src = (int *)source;
  INT num[2], real[2], imag[2];

  if (DT_ISWORD(dtype))
    return src[0];

  switch (DTY(dtype)) {
  case TY_DWORD:
  case TY_INT8:
  case TY_LOG8:
  case TY_DBLE:
    num[1] = src[0];
    num[0] = src[1];
    break;

  case TY_CMPLX:
    num[0] = src[0];
    num[1] = src[1];
    break;

  case TY_DCMPLX:
    real[1] = src[0];
    real[0] = src[1];
    imag[1] = src[2];
    imag[0] = src[3];
    num[0] = getcon(real, DT_DBLE);
    num[1] = getcon(imag, DT_DBLE);
    break;

  case TY_CHAR:
    return getstring(source, size_of(dtype));

  default:
    interr("transfer_load: unexpected dtype", dtype, 3);
  }

  return getcon(num, dtype);
}

static CONST *
eval_transfer(CONST *arg, int dtype)
{
  CONST *src = eval_init_expr(arg);
  CONST *rslt;
  int ssize, sdtype, rsize, rdtype;
  int need, avail;
  char value[256];
  char *buffer = value;
  char *bp;
  INT pad;

  /* Find type and size of the source and result. */
  sdtype = DDTG(src->dtype);
  ssize = size_of(sdtype);
  rdtype = DDTG(dtype);
  rsize = size_of(rdtype);

  /* Be sure we have enough space. */
  need = (rsize > ssize ? rsize : ssize) * 2;
  if (sizeof(value) < need) {
    NEW(buffer, char, need);
    if (buffer == NULL)
      return CONST_ERR(dtype);
  }

  /* Get pad value in case we have to fill. */
  if (DTY(sdtype) == TY_CHAR)
    memset(buffer, ' ', ssize);
  else
    BZERO(buffer, char, ssize);
  pad = transfer_load(sdtype, buffer);

  if (src->id == AC_ACONST)
    src = src->subc;
  bp = buffer;
  avail = 0;
  if (DTY(dtype) != TY_ARRAY) {
    /* Result is scalar. */
    while (avail < rsize) {
      if (src) {
        transfer_store(src->u1.conval, sdtype, bp);
        src = src->next;
      } else
        transfer_store(pad, sdtype, bp);
      bp += ssize;
      avail += ssize;
    }
    rslt = (CONST *)getitem(4, sizeof(CONST));
    BZERO(rslt, CONST, 1);
    rslt->id = AC_CONST;
    rslt->dtype = rdtype;
    rslt->u1.conval = transfer_load(rdtype, buffer);
    rslt->repeatc = 1;
  } else {
    /* Result is array. */
    CONST *root, **current;
    ISZ_T i, nelem;
    int j, cons;

    cons = AD_NUMELM(AD_DPTR(dtype));
    assert(STYPEG(cons) == ST_CONST, "eval_transfer: nelem not const", dtype,
           3);
    nelem = ad_val_of(cons);
    root = NULL;
    current = &root;
    for (i = 0; i < nelem; i++) {
      while (avail < rsize) {
        if (src) {
          transfer_store(src->u1.conval, sdtype, bp);
          src = src->next;
        } else {
          transfer_store(pad, sdtype, bp);
        }
        bp += ssize;
        avail += ssize;
      }
      rslt = (CONST *)getitem(4, sizeof(CONST));
      BZERO(rslt, CONST, 1);
      rslt->id = AC_CONST;
      rslt->dtype = rdtype;
      rslt->u1.conval = transfer_load(rdtype, buffer);
      rslt->repeatc = 1;
      *current = rslt;
      current = &(rslt->next);
      bp -= rsize;
      avail -= rsize;
      for (j = 0; j < avail; j++)
        buffer[j] = buffer[rsize + j];
    }
    rslt = (CONST *)getitem(4, sizeof(CONST));
    BZERO(rslt, CONST, 1);
    rslt->id = AC_ACONST;
    rslt->dtype = dtype;
    rslt->subc = root;
    rslt->repeatc = 1;
  }

  if (buffer != value)
    FREE(buffer);
  return rslt;
}

static CONST *
eval_sqrt(CONST *arg, int dtype)
{
  CONST *rslt = eval_init_expr_item(arg);
  CONST *wrkarg;
  INT conval;

  wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);
  for (; wrkarg; wrkarg = wrkarg->next) {
    INT num1[4];
    INT res[4];
    INT con1;

    con1 = wrkarg->u1.conval;
    switch (DTY(wrkarg->dtype)) {
    case TY_REAL:
      xfsqrt(con1, &res[0]);
      conval = res[0];
      break;
    case TY_DBLE:
      num1[0] = CONVAL1G(con1);
      num1[1] = CONVAL2G(con1);
      xdsqrt(num1, res);
      conval = getcon(res, DT_DBLE);
      break;
    case TY_CMPLX:
    case TY_DCMPLX:
      /*
          a = sqrt(real**2 + imag**2);  "hypot(real,imag)
          if (a == 0) {
              x = 0;
              y = 0;
          }
          else if (real > 0) {
              x = sqrt(0.5 * (a + real));
              y = 0.5 * (imag / x);
          }
          else {
              y = sqrt(0.5 * (a - real));
              if (imag < 0)
                  y = -y;
              x = 0.5 * (imag / y);
          }
          res.real = x;
          res.imag = y;
      */

      error(155, 3, gbl.lineno, "Intrinsic not supported in initialization:",
            "sqrt");
      break;
    default:
      error(155, 3, gbl.lineno, "Intrinsic not supported in initialization:",
            "sqrt");
      break;
    }
    conval = cngcon(conval, wrkarg->dtype, dtype);
    wrkarg->u1.conval = conval;
    wrkarg->dtype = dtype;
    wrkarg->id = AC_CONST;
    wrkarg->repeatc = 1;
  }
  return rslt;
}

/*---------------------------------------------------------------------*/

#define FPINTRIN1(iname, ent, fscutil, dscutil)                     \
  static CONST *ent(CONST *arg, int dtype)                          \
  {                                                                 \
    CONST *rslt = eval_init_expr_item(arg);                         \
    CONST *wrkarg;                                                  \
    INT conval;                                                     \
    wrkarg = (rslt->id == AC_ACONST ? rslt->subc : rslt);           \
    for (; wrkarg; wrkarg = wrkarg->next) {                         \
      INT num1[4];                                                  \
      INT res[4];                                                   \
      INT con1;                                                     \
      con1 = wrkarg->u1.conval;                                     \
      switch (DTY(wrkarg->dtype)) {                                 \
      case TY_REAL:                                                 \
        fscutil(con1, &res[0]);                                     \
        conval = res[0];                                            \
        break;                                                      \
      case TY_DBLE:                                                 \
        num1[0] = CONVAL1G(con1);                                   \
        num1[1] = CONVAL2G(con1);                                   \
        dscutil(num1, res);                                         \
        conval = getcon(res, DT_DBLE);                              \
        break;                                                      \
      case TY_CMPLX:                                                \
      case TY_DCMPLX:                                               \
        error(155, 3, gbl.lineno,                                   \
              "Intrinsic not supported in initialization:", iname); \
        break;                                                      \
      default:                                                      \
        error(155, 3, gbl.lineno,                                   \
              "Intrinsic not supported in initialization:", iname); \
        break;                                                      \
      }                                                             \
      conval = cngcon(conval, wrkarg->dtype, dtype);                \
      wrkarg->u1.conval = conval;                                   \
      wrkarg->dtype = dtype;                                        \
      wrkarg->id = AC_CONST;                                        \
      wrkarg->repeatc = 1;                                          \
    }                                                               \
    return rslt;                                                    \
  }

FPINTRIN1("exp", eval_exp, xfexp, xdexp)

FPINTRIN1("log", eval_log, xflog, xdlog)

FPINTRIN1("log10", eval_log10, xflog10, xdlog10)

FPINTRIN1("sin", eval_sin, xfsin, xdsin)

FPINTRIN1("cos", eval_cos, xfcos, xdcos)

FPINTRIN1("tan", eval_tan, xftan, xdtan)

FPINTRIN1("asin", eval_asin, xfasin, xdasin)

FPINTRIN1("acos", eval_acos, xfacos, xdacos)

FPINTRIN1("atan", eval_atan, xfatan, xdatan)

#define FPINTRIN2(iname, ent, fscutil, dscutil)                     \
  static CONST *ent(CONST *arg, int dtype)                          \
  {                                                                 \
    CONST *rslt;                                                    \
    CONST *arg1, *arg2;                                             \
    INT conval;                                                     \
    arg1 = eval_init_expr_item(arg);                                \
    arg2 = eval_init_expr_item(arg->next);                          \
    rslt = clone_init_const_list(arg1, TRUE);                       \
    arg1 = (rslt->id == AC_ACONST ? rslt->subc : rslt);             \
    arg2 = (arg2->id == AC_ACONST ? arg2->subc : arg2);             \
    for (; arg1; arg1 = arg1->next, arg2 = arg2->next) {            \
      INT num1[4], num2[4];                                         \
      INT res[4];                                                   \
      INT con1, con2;                                               \
      con1 = arg1->u1.conval;                                       \
      con2 = arg2->u1.conval;                                       \
      switch (DTY(arg1->dtype)) {                                   \
      case TY_REAL:                                                 \
        fscutil(con1, con2, &res[0]);                               \
        conval = res[0];                                            \
        break;                                                      \
      case TY_DBLE:                                                 \
        num1[0] = CONVAL1G(con1);                                   \
        num1[1] = CONVAL2G(con1);                                   \
        num2[0] = CONVAL1G(con2);                                   \
        num2[1] = CONVAL2G(con2);                                   \
        dscutil(num1, num2, res);                                   \
        conval = getcon(res, DT_DBLE);                              \
        break;                                                      \
      case TY_CMPLX:                                                \
      case TY_DCMPLX:                                               \
        error(155, 3, gbl.lineno,                                   \
              "Intrinsic not supported in initialization:", iname); \
        break;                                                      \
      default:                                                      \
        error(155, 3, gbl.lineno,                                   \
              "Intrinsic not supported in initialization:", iname); \
        break;                                                      \
      }                                                             \
      conval = cngcon(conval, arg1->dtype, dtype);                  \
      arg1->u1.conval = conval;                                     \
      arg1->dtype = dtype;                                          \
      arg1->id = AC_CONST;                                          \
      arg1->repeatc = 1;                                            \
    }                                                               \
    return rslt;                                                    \
  }

FPINTRIN2("atan2", eval_atan2, xfatan2, xdatan2)

/*---------------------------------------------------------------------*/

static CONST *
eval_init_op(int op, CONST *lop, int ldtype, CONST *rop, int rdtype, int sptr,
             int dtype)
{
  CONST *root = NULL;
  CONST *roottail = NULL;
  CONST *c;
  CONST *cur_lop;
  CONST *cur_rop;
  int dt = DDTG(dtype);
  int e_dtype;
  int i;
  ISZ_T l_repeatc;
  ISZ_T r_repeatc;
  INT l_conval;
  INT r_conval;
  int lsptr;
  int rsptr;
  char *s;
  int llen;
  int rlen;

  if (op == AC_NEG || op == AC_LNOT) {
    cur_lop = (lop->id == AC_ACONST ? lop->subc : lop);
    for (; cur_lop; cur_lop = cur_lop->next) {
      c = (CONST *)getitem(4, sizeof(CONST));
      BZERO(c, CONST, 1);
      c->id = AC_CONST;
      c->dtype = dt;
      c->repeatc = 1;
      l_conval = cur_lop->u1.conval;
      if (dt != cur_lop->dtype) {
        l_conval = cngcon(l_conval, DDTG(cur_lop->dtype), dt);
      }
      if (op == AC_LNOT)
        c->u1.conval = ~(l_conval);
      else
        c->u1.conval = init_negate_const(l_conval, dt);
      add_to_list(c, &root, &roottail);
    }
  } else if (op == AC_ARRAYREF) {
    root = eval_const_array_section(lop, ldtype, dtype);
  } else if (op == AC_CONV) {
    cur_lop = (lop->id == AC_ACONST ? lop->subc : lop);
    l_repeatc = cur_lop->repeatc;
    for (; cur_lop;) {
      c = (CONST *)getitem(4, sizeof(CONST));
      BZERO(c, CONST, 1);
      c->id = AC_CONST;
      c->dtype = dt;
      c->repeatc = 1;
      c->u1.conval = cngcon(cur_lop->u1.conval, DDTG(ldtype), DDTG(dtype));
      add_to_list(c, &root, &roottail);
      if (--l_repeatc <= 0) {
        cur_lop = cur_lop->next;
        if (cur_lop) {
          l_repeatc = cur_lop->repeatc;
        }
      }
    }
  } else if (op == AC_MEMBR_SEL) {
    c = eval_init_expr(lop);
    for (i = rop->u1.conval, cur_lop = c->subc; i > 0 && cur_lop;
         i--, cur_lop = cur_lop->next)
      ;
    if (!cur_lop) {
      interr("Malformed member select opeator", op, 3);
      return CONST_ERR(dtype);
    }
    root = clone_init_const(cur_lop, TRUE);
    root->next = NULL;
  } else if (op == AC_CAT && DTY(ldtype) != TY_ARRAY &&
             DTY(rdtype) != TY_ARRAY) {
    lsptr = lop->u1.conval;
    rsptr = rop->u1.conval;
    llen = size_of(DDTG(ldtype));
    rlen = size_of(DDTG(rdtype));
    s = getitem(0, llen + rlen);
    BCOPY(s, stb.n_base + CONVAL1G(lsptr), char, llen);
    BCOPY(s + llen, stb.n_base + CONVAL1G(rsptr), char, rlen);

    c = (CONST *)getitem(4, sizeof(CONST));
    BZERO(c, CONST, 1);
    c->id = AC_CONST;
    c->dtype = get_type(2, TY_CHAR, llen + rlen); /* should check char type */
    c->repeatc = 1;
    c->u1.conval = c->sptr = getstring(s, llen + rlen);
    add_to_list(c, &root, &roottail);
  } else if (op == AC_INTR_CALL) {
    switch (lop->u1.conval) {
    case AC_I_adjustl:
      root = eval_adjustl(rop, dtype);
      break;
    case AC_I_adjustr:
      root = eval_adjustr(rop, dtype);
      break;
    case AC_I_char:
      root = eval_char(rop, dtype);
      break;
    case AC_I_ichar:
      root = eval_ichar(rop, dtype);
      break;
    case AC_I_index:
      root = eval_index(rop, dtype);
      break;
    case AC_I_int:
      root = eval_int(rop, dtype);
      break;
    case AC_I_ishft:
      root = eval_ishft(rop, dtype);
      break;
    case AC_I_len_trim:
      root = eval_len_trim(rop, dtype);
      break;
    case AC_I_ubound:
    case AC_I_lbound:
      root = eval_ul_bound(lop->u1.conval, rop, dtype);
      break;
    case AC_I_min:
      root = eval_min(rop, dtype);
      break;
    case AC_I_max:
      root = eval_max(rop, dtype);
      break;
    case AC_I_nint:
      root = eval_nint(rop, dtype);
      break;
    case AC_I_fltconvert:
      root = eval_fltconvert(rop, dtype);
      break;
    case AC_I_repeat:
      root = eval_repeat(rop, dtype);
      break;
    case AC_I_reshape:
      root = eval_reshape(rop, dtype);
      break;
    case AC_I_selected_int_kind:
      root = eval_selected_int_kind(rop, dtype);
      break;
    case AC_I_selected_real_kind:
      root = eval_selected_real_kind(rop, dtype);
      break;
    case AC_I_selected_char_kind:
      root = eval_selected_char_kind(rop, dtype);
      break;
    case AC_I_scan:
      root = eval_scan(rop, dtype);
      break;
    case AC_I_shape:
      root = eval_shape(rop, dtype);
      break;
    case AC_I_size:
      root = eval_size(rop, dtype);
      break;
    case AC_I_trim:
      root = eval_trim(rop, dtype);
      break;
    case AC_I_verify:
      root = eval_verify(rop, dtype);
      break;
    case AC_I_floor:
      root = eval_floor(rop, dtype);
      break;
    case AC_I_ceiling:
      root = eval_ceiling(rop, dtype);
      break;
    case AC_I_mod:
      root = eval_mod(rop, dtype);
      break;
    case AC_I_null:
      root = eval_null(rop, dtype);
      break;
    case AC_I_transfer:
      root = eval_transfer(rop, dtype);
      break;
    case AC_I_sqrt:
      root = eval_sqrt(rop, dtype);
      break;
    case AC_I_exp:
      root = eval_exp(rop, dtype);
      break;
    case AC_I_log:
      root = eval_log(rop, dtype);
      break;
    case AC_I_log10:
      root = eval_log10(rop, dtype);
      break;
    case AC_I_sin:
      root = eval_sin(rop, dtype);
      break;
    case AC_I_cos:
      root = eval_cos(rop, dtype);
      break;
    case AC_I_tan:
      root = eval_tan(rop, dtype);
      break;
    case AC_I_asin:
      root = eval_asin(rop, dtype);
      break;
    case AC_I_acos:
      root = eval_acos(rop, dtype);
      break;
    case AC_I_atan:
      root = eval_atan(rop, dtype);
      break;
    case AC_I_atan2:
      root = eval_atan2(rop, dtype);
      break;
    case AC_I_abs:
      root = eval_abs(rop, dtype);
      break;
    default:
      interr("eval_init_op: intrinsic not supported in initialiation",
             lop->u1.conval, 3);
      return CONST_ERR(dtype);
    }
  } else if (DTY(ldtype) == TY_ARRAY && DTY(rdtype) == TY_ARRAY) {
    /* array <binop> array */
    cur_lop = (lop->id == AC_ACONST ? lop->subc : lop);
    cur_rop = (rop->id == AC_ACONST ? rop->subc : rop);
    l_repeatc = cur_lop->repeatc;
    r_repeatc = cur_rop->repeatc;
    e_dtype = DDTG(dtype);
    if (op == AC_CAT) {
      for (; cur_rop && cur_lop;) {
        lsptr = cur_lop->u1.conval;
        llen = size_of(DDTG(ldtype));
        rsptr = cur_rop->u1.conval;
        rlen = size_of(DDTG(rdtype));
        s = getitem(0, llen + rlen);
        BCOPY(s, stb.n_base + CONVAL1G(lsptr), char, llen);
        BCOPY(s + llen, stb.n_base + CONVAL1G(rsptr), char, rlen);

        c = (CONST *)getitem(4, sizeof(CONST));
        BZERO(c, CONST, 1);
        c->id = AC_CONST;
        c->dtype = get_type(2, TY_CHAR, llen + rlen);
        c->repeatc = 1;
        c->u1.conval = c->sptr = getstring(s, llen + rlen);

        add_to_list(c, &root, &roottail);
        if (--l_repeatc <= 0) {
          cur_lop = cur_lop->next;
          if (cur_lop) {
            r_repeatc = cur_lop->repeatc;
          }
        }
        if (--r_repeatc <= 0) {
          cur_rop = cur_rop->next;
          if (cur_rop) {
            r_repeatc = cur_rop->repeatc;
          }
        }
      }
      return root;
    }
    for (; cur_rop && cur_lop;) {
      c = (CONST *)getitem(4, sizeof(CONST));
      BZERO(c, CONST, 1);
      c->id = AC_CONST;
      c->dtype = dt;
      c->repeatc = 1;
      l_conval = cur_lop->u1.conval;
      if (DDTG(cur_lop->dtype) != e_dtype) {
        l_conval = cngcon(l_conval, DDTG(cur_lop->dtype), e_dtype);
      }
      r_conval = cur_rop->u1.conval;
      switch (get_ast_op(op)) {
      case OP_XTOI:
      case OP_XTOK:
      case OP_XTOX:
        /* the front-end sets the correct type for the right operand */
        break;
      default:
        if (DDTG(cur_rop->dtype) != e_dtype) {
          r_conval = cngcon(r_conval, DDTG(cur_rop->dtype), e_dtype);
        }
        break;
      }
      c->u1.conval = init_fold_const(get_ast_op(op), l_conval, r_conval, dt);
      add_to_list(c, &root, &roottail);
      if (--l_repeatc <= 0) {
        cur_lop = cur_lop->next;
        if (cur_lop) {
          l_repeatc = cur_lop->repeatc;
        }
      }
      if (--r_repeatc <= 0) {
        cur_rop = cur_rop->next;
        if (cur_rop) {
          r_repeatc = cur_rop->repeatc;
        }
      }
    }
  } else if (DTY(ldtype) == TY_ARRAY) {
    /* array <binop> scalar */
    cur_lop = (lop->id == AC_ACONST ? lop->subc : lop);
    l_repeatc = cur_lop->repeatc;
    e_dtype = DDTG(dtype);
    r_conval = rop->u1.conval;
    switch (get_ast_op(op)) {
    case OP_XTOI:
    case OP_XTOK:
    case OP_XTOX:
      /* the front-end sets the correct type for the right operand */
      break;
    case OP_CAT:
      rsptr = rop->u1.conval;
      rlen = size_of(DDTG(rdtype));
      for (; cur_lop;) {
        lsptr = cur_lop->u1.conval;
        llen = size_of(DDTG(ldtype));
        s = getitem(0, llen + rlen);
        BCOPY(s, stb.n_base + CONVAL1G(lsptr), char, llen);
        BCOPY(s + llen, stb.n_base + CONVAL1G(rsptr), char, rlen);

        c = (CONST *)getitem(4, sizeof(CONST));
        BZERO(c, CONST, 1);
        c->id = AC_CONST;
        c->dtype = get_type(2, TY_CHAR, llen + rlen);
        c->repeatc = 1;
        c->u1.conval = c->sptr = getstring(s, llen + rlen);

        add_to_list(c, &root, &roottail);
        if (--l_repeatc <= 0) {
          cur_lop = cur_lop->next;
          if (cur_lop) {
            l_repeatc = cur_lop->repeatc;
          }
        }
      }
      return root;
      break;
    default:
      if (rop->dtype != e_dtype) {
        r_conval = cngcon(r_conval, rop->dtype, e_dtype);
      }
    }
    for (; cur_lop;) {
      c = (CONST *)getitem(4, sizeof(CONST));
      BZERO(c, CONST, 1);
      c->id = AC_CONST;
      c->dtype = dt;
      c->repeatc = 1;
      l_conval = cur_lop->u1.conval;
      if (DDTG(cur_lop->dtype) != e_dtype) {
        l_conval = cngcon(l_conval, DDTG(cur_lop->dtype), e_dtype);
      }
      c->u1.conval = init_fold_const(get_ast_op(op), l_conval, r_conval, dt);
      add_to_list(c, &root, &roottail);
      if (--l_repeatc <= 0) {
        cur_lop = cur_lop->next;
        if (cur_lop) {
          l_repeatc = cur_lop->repeatc;
        }
      }
    }
  } else if (DTY(rdtype) == TY_ARRAY) {
    /* scalar <binop> array */
    cur_rop = (rop->id == AC_ACONST ? rop->subc : rop);
    r_repeatc = cur_rop->repeatc;
    e_dtype = DDTG(dtype);
    l_conval = lop->u1.conval;
    if (lop->dtype != e_dtype) {
      l_conval = cngcon(l_conval, lop->dtype, e_dtype);
    }
    if (get_ast_op(op) == OP_CAT) {
      lsptr = lop->u1.conval;
      llen = size_of(DDTG(ldtype));
      for (; cur_rop;) {
        rsptr = cur_rop->u1.conval;
        rlen = size_of(DDTG(rdtype));
        s = getitem(0, llen + rlen);
        BCOPY(s, stb.n_base + CONVAL1G(lsptr), char, llen);
        BCOPY(s + llen, stb.n_base + CONVAL1G(rsptr), char, rlen);

        c = (CONST *)getitem(4, sizeof(CONST));
        BZERO(c, CONST, 1);
        c->id = AC_CONST;
        c->dtype = get_type(2, TY_CHAR, llen + rlen);
        c->repeatc = 1;
        c->u1.conval = c->sptr = getstring(s, llen + rlen);

        add_to_list(c, &root, &roottail);
        if (--r_repeatc <= 0) {
          cur_rop = cur_rop->next;
          if (cur_rop) {
            r_repeatc = cur_rop->repeatc;
          }
        }
      }
      return root;
    }
    for (; cur_rop;) {
      c = (CONST *)getitem(4, sizeof(CONST));
      BZERO(c, CONST, 1);
      c->id = AC_CONST;
      c->dtype = dt;
      c->repeatc = 1;
      r_conval = cur_rop->u1.conval;
      switch (get_ast_op(op)) {
      case OP_XTOI:
      case OP_XTOK:
      case OP_XTOX:
        /* the front-end sets the correct type for the right operand */
        break;
      default:
        if (DDTG(cur_rop->dtype) != e_dtype) {
          r_conval = cngcon(r_conval, DDTG(cur_rop->dtype), e_dtype);
        }
      }
      c->u1.conval = init_fold_const(get_ast_op(op), l_conval, r_conval, dt);
      add_to_list(c, &root, &roottail);
      if (--r_repeatc <= 0) {
        cur_rop = cur_rop->next;
        if (cur_rop) {
          r_repeatc = cur_rop->repeatc;
        }
      }
    }
  } else {
    /* scalar <binop> scalar */
    root = (CONST *)getitem(4, sizeof(CONST));
    BZERO(root, CONST, 1);
    root->id = AC_CONST;
    root->repeatc = 1;
    root->dtype = dt;
    op = get_ast_op(op);
    switch (op) {
    case OP_EQ:
    case OP_GE:
    case OP_GT:
    case OP_LE:
    case OP_LT:
    case OP_NE:
      l_conval =
          init_fold_const(OP_CMP, lop->u1.conval, rop->u1.conval, ldtype);
      switch (op) {
      case OP_EQ:
        l_conval = (l_conval == 0);
        break;
      case OP_GE:
        l_conval = (l_conval >= 0);
        break;
      case OP_GT:
        l_conval = (l_conval > 0);
        break;
      case OP_LE:
        l_conval = (l_conval <= 0);
        break;
      case OP_LT:
        l_conval = (l_conval < 0);
        break;
      case OP_NE:
        l_conval = (l_conval != 0);
        break;
      }
      l_conval = l_conval ? SCFTN_TRUE : SCFTN_FALSE;
      root->u1.conval = l_conval;
      break;
    case OP_LEQV:
      l_conval =
          init_fold_const(OP_CMP, lop->u1.conval, rop->u1.conval, ldtype);
      root->u1.conval = (l_conval == 0);
      break;
    case OP_LNEQV:
      l_conval =
          init_fold_const(OP_CMP, lop->u1.conval, rop->u1.conval, ldtype);
      root->u1.conval = (l_conval != 0);
      break;
    case OP_LOR:
      root->u1.conval = lop->u1.conval | rop->u1.conval;
      break;
    case OP_LAND:
      root->u1.conval = lop->u1.conval & rop->u1.conval;
      break;
    case OP_XTOI:
    case OP_XTOK:
      root->u1.conval = init_fold_const(op, lop->u1.conval, rop->u1.conval, dt);
      break;
    default:
      l_conval = lop->u1.conval;
      r_conval = rop->u1.conval;
      if (lop->dtype != dt)
        l_conval = cngcon(l_conval, lop->dtype, dt);
      if (rop->dtype != dt)
        r_conval = cngcon(r_conval, rop->dtype, dt);
      root->u1.conval = init_fold_const(op, l_conval, r_conval, dt);
      break;
    }
  }
  return root;
}

static CONST *
convert_acl_dtype(CONST *head, int oldtype, int newtype)
{
  int dtype;

  CONST *cur_lop;
  if (DTY(oldtype) == TY_STRUCT || DTY(oldtype) == TY_CHAR ||
      DTY(oldtype) == TY_NCHAR || DTY(oldtype) == TY_UNION) {
    return head;
  }
  cur_lop = head;
  dtype = DDTG(newtype);

  /* make sure all are AC_CONST */
  for (cur_lop = head; cur_lop; cur_lop = cur_lop->next) {
    if (cur_lop->id != AC_CONST)
      return head;
  }

  for (cur_lop = head; cur_lop; cur_lop = cur_lop->next) {
    if (cur_lop->dtype != dtype) {
      cur_lop->u1.conval = cngcon(cur_lop->u1.conval, cur_lop->dtype, dtype);
      cur_lop->dtype = dtype;
    }
  }
  return head;
}

static CONST *
eval_array_constructor(CONST *e)
{
  CONST *root = NULL;
  CONST *roottail = NULL;
  CONST *cur_e;
  CONST *new_e;

  /* collapse nested array contstructors */
  for (cur_e = e->subc; cur_e; cur_e = cur_e->next) {
    if (cur_e->id == AC_ACONST) {
      new_e = eval_array_constructor(cur_e);
    } else {
      new_e = eval_init_expr_item(cur_e);
      if (new_e && new_e->id == AC_ACONST) {
        new_e = eval_array_constructor(new_e);
      }
    }
    add_to_list(new_e, &root, &roottail);
  }
  return root;
}

static CONST *
eval_init_expr_item(CONST *cur_e)
{
  CONST *root = NULL;
  CONST *new_e, *rslt, *rslttail;
  CONST *lop;
  CONST *rop, *temp;
  int sptr, repeatc;

  switch (cur_e->id) {
  case AC_IDENT:
    if (PARAMG(cur_e->sptr) || (DOVARG(cur_e->sptr) && DINITG(cur_e->sptr)) ||
        (CCSYMG(cur_e->sptr) && DINITG(cur_e->sptr))) {
      new_e =
          clone_init_const_list(init_const[PARAMVALG(cur_e->sptr) - 1], TRUE);
      if (cur_e->mbr) {
        new_e->sptr = cur_e->mbr;
      }
    }
    break;
  case AC_CONST:
    new_e = clone_init_const(cur_e, TRUE);
    break;
  case AC_IEXPR:
    if (cur_e->u1.expr.op != AC_INTR_CALL) {
      lop = eval_init_expr(cur_e->u1.expr.lop);
      temp = cur_e->u1.expr.rop;
      if (temp && cur_e->u1.expr.op == AC_ARRAYREF &&
          temp->u1.expr.op == AC_TRIPLE) {
        rop = eval_const_array_triple_section(temp);
      } else
        rop = eval_init_expr(temp);
      new_e = eval_init_op(cur_e->u1.expr.op, lop, cur_e->u1.expr.lop->dtype,
                           rop, rop ? cur_e->u1.expr.rop->dtype : 0,
                           cur_e->sptr, cur_e->dtype);
    } else {
      new_e = eval_init_op(cur_e->u1.expr.op, cur_e->u1.expr.lop,
                           cur_e->u1.expr.lop->dtype, cur_e->u1.expr.rop,
                           cur_e->u1.expr.rop ? cur_e->u1.expr.rop->dtype : 0,
                           cur_e->sptr, cur_e->dtype);
    }
    if (cur_e->repeatc > 1) {
      /* need to copy all ict as many times as repeatc*/
      repeatc = cur_e->repeatc;
      rslt = new_e;
      rslttail = new_e;
      while (repeatc > 1) {
        new_e = clone_init_const_list(new_e, TRUE);
        add_to_list(new_e, &rslt, &rslttail);
        --repeatc;
      }
      new_e = rslt;
    }
    new_e->sptr = cur_e->sptr;
    break;
  case AC_ACONST:
    new_e = clone_init_const(cur_e, TRUE);
    new_e->subc = eval_array_constructor(cur_e);
    if (new_e->subc)
      new_e->subc = convert_acl_dtype(new_e->subc, DDTG(new_e->subc->dtype),
                                      DDTG(new_e->dtype));
    break;
  case AC_SCONST:
    new_e = clone_init_const(cur_e, TRUE);
    new_e->subc = eval_init_expr(new_e->subc);
    if (new_e->subc->dtype == cur_e->dtype) {
      new_e->subc = new_e->subc->subc;
    }
    break;
  case AC_IDO:
    new_e = eval_do(cur_e);
    break;
  }

  return new_e;
}

static CONST *
eval_init_expr(CONST *e)
{
  CONST *root = NULL;
  CONST *roottail = NULL;
  CONST *cur_e;
  CONST *new_e;
  CONST *r;
  CONST *t;
  CONST *subc;

  for (cur_e = e; cur_e; cur_e = cur_e->next) {
    switch (cur_e->id) {
    case AC_SCONST:
      new_e = clone_init_const(cur_e, TRUE);
      new_e->subc = eval_init_expr(new_e->subc);
      if (new_e->subc->dtype == cur_e->dtype) {
        new_e->subc = new_e->subc->subc;
      }
      break;
    case AC_ACONST:
      new_e = clone_init_const(cur_e, TRUE);
      new_e->subc = eval_array_constructor(cur_e);
      if (new_e->subc)
        new_e->subc = convert_acl_dtype(new_e->subc, DDTG(new_e->subc->dtype),
                                        DDTG(new_e->dtype));
      break;
    case AC_IDENT:
      /* need this for AC_MEMBR_SEL */
      if (cur_e->sptr && DTY(DTYPEG(cur_e->sptr)) == TY_ARRAY) {
        new_e = clone_init_const(cur_e, TRUE);
        new_e->subc = eval_init_expr_item(cur_e);
        new_e->sptr = 0;
        new_e->id = AC_ACONST;
        break;
      }
    default:
      new_e = eval_init_expr_item(cur_e);
      break;
    }
    add_to_list(new_e, &root, &roottail);
  }

  return root;
}

static CONST *
eval_do(CONST *ido)
{
  ISZ_T i;
  IDOINFO *di = &ido->u1.ido;
  int idx_sptr = di->index_var;
  CONST *idx_ict;
  CONST *root = NULL;
  CONST *roottail = NULL;
  CONST *ict;
  CONST *initict = eval_init_expr_item(di->initval);
  CONST *limitict = eval_init_expr_item(di->limitval);
  CONST *stepict = eval_init_expr_item(di->stepval);
  ISZ_T initval = get_ival(initict->dtype, initict->u1.conval);
  ISZ_T limitval = get_ival(limitict->dtype, limitict->u1.conval);
  ISZ_T stepval = get_ival(stepict->dtype, stepict->u1.conval);
  INT num[2];
  int inflag = 0;

  if (DINITG(idx_sptr) && PARAMVALG(idx_sptr)) {
    idx_ict = init_const[PARAMVALG(idx_sptr) - 1];
  } else {
    idx_ict = (CONST *)getitem(4, sizeof(CONST));
    BZERO(idx_ict, CONST, 1);
    idx_ict->id = AC_CONST;
    idx_ict->dtype = DTYPEG(idx_sptr);
    idx_ict->repeatc = 1;
    save_init(idx_ict, idx_sptr);
    DINITP(idx_sptr, 1); /* MORE use some other flag??? */
  }

  DOVARP(idx_sptr, 1);
  if (stepval >= 0) {
    for (i = initval; i <= limitval; i += stepval) {
      switch (DTY(idx_ict->dtype)) {
      case TY_INT8:
      case TY_LOG8:
        ISZ_2_INT64(i, num);
        idx_ict->u1.conval = getcon(num, idx_ict->dtype);
        break;
      default:
        idx_ict->u1.conval = i;
        break;
      }
      ict = eval_init_expr(ido->subc);
      add_to_list(ict, &root, &roottail);
      inflag = 1;
    }
  } else {
    for (i = initval; i >= limitval; i += stepval) {
      switch (DTY(idx_ict->dtype)) {
      case TY_INT8:
      case TY_LOG8:
        ISZ_2_INT64(i, num);
        idx_ict->u1.conval = getcon(num, idx_ict->dtype);
        break;
      default:
        idx_ict->u1.conval = i;
        break;
      }
      ict = eval_init_expr(ido->subc);
      add_to_list(ict, &root, &roottail);
      inflag = 1;
    }
  }
  if (inflag == 0 && ido->subc) {
    ict = eval_init_expr(ido->subc);
    add_to_list(ict, &root, &roottail);
  }
  DOVARP(idx_sptr, 0);

  return root;
}

static void
replace_const(CONST *old, CONST *replacment)
{
  CONST *oldnext = old->next;
  CONST *ict;
  CONST *last;

  ict = clone_init_const_list(replacment, TRUE);

  for (last = ict; last->next; last = last->next)
    ;
  last->next = oldnext;

  *old = *ict;
}

static CONST *
clone_init_const(CONST *original, int temp)
{
  CONST *clone;

  if (!original)
    return NULL;
  clone = (CONST *)getitem(4, sizeof(CONST));
  *clone = *original;
  if (clone->subc) {
    clone->subc = clone_init_const_list(original->subc, temp);
  }
  if (clone->id == AC_IEXPR) {
    if (clone->u1.expr.lop) {
      clone->u1.expr.lop = clone_init_const_list(original->u1.expr.lop, temp);
    }
    if (clone->u1.expr.rop) {
      clone->u1.expr.rop = clone_init_const_list(original->u1.expr.rop, temp);
    }
  }
  clone->next = NULL;
  return clone;
}

static CONST *
clone_init_const_list(CONST *original, int temp)
{
  CONST *clone = NULL;
  CONST *clonetail = NULL;

  clone = clone_init_const(original, temp);
  for (original = original->next; original; original = original->next) {
    add_to_list(clone_init_const(original, temp), &clone, &clonetail);
  }
  return clone;
}

static void
add_to_list(CONST *val, CONST **root, CONST **roottail)
{
  CONST *tail;
  if (roottail && *roottail) {
    (*roottail)->next = val;
  } else if (*root) {
    for (tail = *root; tail->next; tail = tail->next)
      ;
    tail->next = val;
  } else {
    *root = val;
  }
  if (roottail && val) { /* find and save the end of the list */
    for (tail = val; tail->next; tail = tail->next)
      ;
    *roottail = tail;
  }
}

static void
save_init(CONST *ict, int sptr)
{
  if (PARAMVALG(sptr)) {
    /* multiple initialization or overlapping initialization error,
     * recognized and reported in assem.c */
    return;
  }

  if (cur_init >= init_list_count) {
    interr("Saved initializer list overflow", init_list_count, 3);
    return;
  }
  init_const[cur_init] = ict;
  PARAMVALP(sptr, ++cur_init); /* paramval is cardinal */
}

static void
dmp_saved_init(int sptr, int save_idx)
{
  int i;
  FILE *dfile;

  dfile = gbl.dbgfil ? gbl.dbgfil : stderr;
  fprintf(dfile, "Init for %s (%d) saved in init_const[%d]:\n", SYMNAME(sptr),
          sptr, save_idx);
  dmp_const(init_const[save_idx], 1);
}
