/*
 * Copyright (C) 2007 Reinhard Prix
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with with program; see the file COPYING. If not, write to the 
 *  Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, 
 *  MA  02111-1307  USA
 */

/**
 * \author Reinhard Prix
 * \date 2006
 * \file
 * \brief Functions for optimal lattice-covering of Doppler-spaces.
 * NOTE: this module always uses *ECLIPTIC* coordinates for interal operations
 */

/*---------- INCLUDES ----------*/
#include <math.h>
#include <gsl/gsl_sys.h>

#include <lal/LALStdlib.h>
#include <lal/DetectorSite.h>
#include <lal/LALError.h>
#include <lal/LatticeCovering.h>
#include <lal/LogPrintf.h>

#include "FlatPulsarMetric.h"

#include "DopplerFullScan.h"
#include "DopplerLatticeCovering.h"

/*---------- DEFINES ----------*/
NRCSID( DOPPLERLATTICECOVERING, "$Id$" );

#define EPS_REAL8	1e-10	/**< relative error used in REAL8 comparisons */

#define TRUE (1==1)
#define FALSE (1==0)

#define MIN(x,y) (x < y ? x : y)
#define MAX(x,y) (x > y ? x : y)
#define SIGN(x) ( x < 0 ? -1 : ( x > 0 ? 1 : 0 ) )

#define SQUARE(x) ( (x) * (x) )
#define VECT_NORM(x) sqrt( SQUARE((x)[0]) + SQUARE((x)[1]) + SQUARE((x)[2]) )
#define VECT_ADD(x,y) do { (x)[0] += (y)[0]; (x)[1] += (y)[1]; (x)[2] += (y)[2]; } while(0)
#define VECT_MULT(x,k) do { (x)[0] *= k; (x)[1] *= k; (x)[2] *= k; } while(0)
#define VECT_COPY(dst,src) do { (dst)[0] = (src)[0]; (dst)[1] = (src)[1]; (dst)[2] = (src)[2]; } while(0)

#define VECT_HEMI(x) ( (x)[2] < 0 ? HEMI_SOUTH : ( (x)[2] > 0 ? HEMI_NORTH : 0 ) )

/*---------- internal types ----------*/
typedef enum {
  HEMI_BOTH  =  0,	/**< points lie on both hemispheres */
  HEMI_NORTH =  1,	/**< all points on northern hemisphere */
  HEMI_SOUTH =  2	/**< all points on southern hemisphere */
} hemisphere_t;

typedef REAL8 vect2D_t[2];	/**< 2D vector */
typedef REAL8 vect3D_t[3];	/**< 3D vector */

/** 2D-polygon of points {nX, nY} on a single hemisphere of ecliptic sky-sphere */
typedef struct {
  UINT4 length;			/**< number of elements */
  vect2D_t *data;		/**< array of 2D vectors */
} vect2Dlist_t;

/** List of 3D vectors */
typedef struct {
  UINT4 length;			/**< number of elements */
  vect3D_t *data;		/**< array of 3D vectors */
} vect3Dlist_t;

/** 'standard' representation of Doppler-paremeters */
typedef struct {
  vect3D_t vn;			/**< unit-vector pointing to sky-location */
  PulsarSpins fkdot;		/**< vector of spins f^(k) */
} dopplerParams_t;

/** boundary of (single-hemisphere) search region in Doppler-space */
typedef struct {
  vect2Dlist_t skyRegion; 	/**< (ecliptic) vector-polygon {nX, nY} defining a sky search-region */
  hemisphere_t hemisphere;	/**< which sky-hemisphere the polygon lies on */
  PulsarSpinRange fkRange;	/**< search-region in spin parameters ['physical' units] */
} dopplerBoundary_t;

struct tagDopplerLatticeScan {
  scan_state_t state;		/**< current state of the scan: idle, ready of finished */
  REAL8 Tspan;			/**< total observation time spanned */
  UINT4 dimSearch;		/**< dimension of search-space to cover [can be less than dim(latticeOrigin)!] */
  dopplerBoundary_t boundary;	/**< boundary of Doppler-space to cover */
  gsl_vector *latticeOrigin;	/**< 'origin' of the lattice {w0, kX, kY, w1, w2, ... } */
  gsl_matrix *latticeGenerator;	/**< generating matrix for the lattice: rows are the lattice-vectors */
  gsl_vector_int *index;	/**< index-counters of current lattice point */
};

/*---------- empty initializers ---------- */
dopplerParams_t empty_dopplerParams;

/*---------- Global variables ----------*/
extern INT4 lalDebugLevel;

/* ----- EXTERNAL API [FIXME: move to header-file] ----- */
int XLALgetCurrentLatticeIndex ( gsl_vector_int **index, const DopplerLatticeScan *scan  );
int XLALsetCurrentLatticeIndex ( DopplerLatticeScan *scan, const gsl_vector_int *index );
int XLALgetCurrentDopplerPos ( PulsarDopplerParams *pos, const DopplerLatticeScan *scan, CoordinateSystem skyCoords );
int XLALadvanceLatticeIndex ( DopplerLatticeScan *scan );

/*---------- internal function prototypes ----------*/
void skyRegionString2vect3D ( LALStatus *, vect3Dlist_t **skyRegionEcl, const CHAR *skyRegionString );
void setupSearchRegion ( LALStatus *status, DopplerLatticeScan *scan, const DopplerRegion *searchRegion );

hemisphere_t onWhichHemisphere ( const vect3Dlist_t *skypoints );
int skyposToVect3D ( vect3D_t *eclVect, const SkyPosition *skypos );
int vect3DToSkypos ( SkyPosition *skypos, const vect3D_t *vect );
int findCenterOfMass ( vect3D_t *center, const vect3Dlist_t *points );

int indexToCanonicalOffset ( gsl_vector **canonicalOffset, const gsl_vector_int *index, const gsl_matrix *generator );
int XLALindexToDoppler ( dopplerParams_t *doppler, const gsl_vector_int *index, const DopplerLatticeScan *scan );
int isIndexInsideBoundary ( const gsl_vector_int *index, const DopplerLatticeScan *scan );

int convertDoppler2Canonical ( gsl_vector **canonicalPoint, const dopplerParams_t *doppler, REAL8 Tspan );
int convertSpins2Canonical ( PulsarSpins wk, const PulsarSpins fkdot, REAL8 Tspan );
int convertCanonical2Doppler ( dopplerParams_t *doppler, const gsl_vector *canonical, hemisphere_t hemi, REAL8 Tspan );

int vect2DInPolygon ( const vect2D_t *point, const vect2Dlist_t *polygon );
int isDopplerInsideBoundary ( const dopplerParams_t *doppler,  const dopplerBoundary_t *boundary );

/*==================== FUNCTION DEFINITIONS ====================*/

/* ------------------------------------------------------ */
/* -------------------- EXTERNAL API -------------------- */
/* ------------------------------------------------------ */

/** Initialize search-grid using optimal lattice-covering
 */
void
InitDopplerLatticeScan ( LALStatus *status, 
			 DopplerLatticeScan **scan, 		/**< [out] initialized scan-state for lattice-scan */
			 const DopplerLatticeInit *init		/**< [in] scan init parameters */
			 )
{
  DopplerLatticeScan *ret = NULL;
  gsl_matrix *gij;

  INITSTATUS( status, "InitDopplerLatticeScan", DOPPLERLATTICECOVERING );
  ATTATCHSTATUSPTR ( status );

  ASSERT ( scan, status, DOPPLERSCANH_ENULL, DOPPLERSCANH_MSGENULL );  
  ASSERT ( *scan == NULL, status, DOPPLERSCANH_ENONULL, DOPPLERSCANH_MSGENONULL );  
  ASSERT ( init, status, DOPPLERSCANH_ENULL, DOPPLERSCANH_MSGENULL );  

  /* prepare scan-structure to return */
  if ( (ret = LALCalloc ( 1, sizeof(*ret) )) == NULL ) {
    ABORT (status, DOPPLERSCANH_EMEM, DOPPLERSCANH_MSGEMEM);
  }

  /* first store observation-time, required for conversion 'Doppler <--> canonical' */
  ret->Tspan = init->Tspan;

  /* ----- get search-Region ----- */
  TRY ( setupSearchRegion ( status->statusPtr, ret, &(init->searchRegion) ), status );

  /* ----- compute flat metric ----- */
  if ( (gij = gsl_matrix_calloc (ret->dimSearch, ret->dimSearch)) == NULL ) {
    FreeDopplerLatticeScan ( status->statusPtr, scan );
    ABORT (status, DOPPLERSCANH_EMEM, DOPPLERSCANH_MSGEMEM);
  }

  if ( XLALFlatMetricCW ( gij, init->searchRegion.refTime, init->startTime, init->Tspan, init->ephemeris ) )
    {
      gsl_matrix_free ( gij );
      FreeDopplerLatticeScan ( status->statusPtr, scan );
      LALPrintError ("\nCall to XLALFlatMetricCW() failed!\n\n");
      ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );
    }
  fprintf ( stderr, "\ngij = ");
  XLALfprintfGSLmatrix ( stderr, "% 15.9e ", gij );

  /* ----- compute generating matrix for the lattice ----- */
  if ( XLALFindCoveringGenerator (&(ret->latticeGenerator), LATTICE_TYPE_ANSTAR, sqrt(init->metricMismatch), gij ) < 0)
    {
      int code = xlalErrno;
      XLALClearErrno(); 
      gsl_matrix_free ( gij );
      FreeDopplerLatticeScan ( status->statusPtr, scan );
      LALPrintError ("\nERROR: XLALFindCoveringGenerator() failed (xlalErrno = %d)!\n\n", code);
      ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );
    }
  fprintf ( stderr, "\ngenerator = ");
  XLALfprintfGSLmatrix ( stderr, "% 15.9e ", ret->latticeGenerator );

  fprintf ( stderr, "\norigin = ");
  XLALfprintfGSLvector ( stderr, "% .9e ", ret->latticeOrigin );
  fprintf ( stderr, "\n");

  /* ----- prepare index-counter to generate lattice-points ----- */
  if ( (ret->index = gsl_vector_int_calloc ( ret->dimSearch )) == NULL ) {
    gsl_matrix_free ( gij );
    FreeDopplerLatticeScan ( status->statusPtr, scan );
    ABORT (status, DOPPLERSCANH_EMEM, DOPPLERSCANH_MSGEMEM);
  }
  
  /* clean up memory */
  gsl_matrix_free ( gij );

  /* return final scan-state */
  ret->state = STATE_READY;
  (*scan) = ret;

#if 1
  { /* some debug-tests */
    dopplerParams_t doppler = empty_dopplerParams;
    gsl_vector *origin2 = NULL;
    SkyPosition skypos = empty_SkyPosition;
    int inside;
    if ( convertCanonical2Doppler ( &doppler, ret->latticeOrigin, ret->boundary.hemisphere, ret->Tspan ) ) {
      ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );
    }
    inside = isDopplerInsideBoundary ( &doppler, &(ret->boundary) );
    fprintf (stderr, "Doppler inside boundary: %d\n", inside );
    fprintf (stderr, "\norigin: vn = { %f, %f, %f }, fkdot = { %f, %g, %g, %g }\n", 
	     doppler.vn[0], doppler.vn[1], doppler.vn[2],
	     doppler.fkdot[0], doppler.fkdot[1], doppler.fkdot[2], doppler.fkdot[3] 
	     );
    if ( convertDoppler2Canonical ( &origin2, &doppler, ret->Tspan ) ) {
      ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );
    }
    fprintf (stderr, "\norigin2 = ");
    XLALfprintfGSLvector ( stderr, "% .9e ", origin2 );
    fprintf ( stderr, "\n");

    skypos.system = COORDINATESYSTEM_EQUATORIAL;
    if ( vect3DToSkypos ( &skypos, (const vect3D_t*)&(doppler.vn) ) ) {
      ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );
    }
    fprintf (stderr, "\nSkypos: Alpha = %f, Delta = %f\n\n", skypos.longitude, skypos.latitude );
  }
#if 0
  {
    gsl_vector *canOffs = NULL;
    gsl_vector_int_set ( ret->index, 0, 1 );
    fprintf (stderr, "index = ");
    XLALfprintfGSLvector_int ( stderr, "%d", ret->index );
    if ( indexToCanonicalOffset ( &canOffs, ret->index, ret->latticeGenerator ) ) {
      ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );
    }
    fprintf ( stderr, "\noffset = ");
    XLALfprintfGSLvector ( stderr, "% .9e ", canOffs );
    fprintf ( stderr, "\n");
    gsl_vector_free ( canOffs );
  }
#endif

  {
    PulsarDopplerParams dopplerpos;
    if ( XLALgetCurrentDopplerPos ( &dopplerpos, ret, COORDINATESYSTEM_EQUATORIAL ) ) {
      ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );    
    }
    printf ("\nCurrent Doppler-position: {Freq, Alpha, Delta, f1dot, f2dot, f3dot} = {%f, %f, %f, %g, %g, %g}\n\n",
	    dopplerpos.fkdot[0], dopplerpos.Alpha, dopplerpos.Delta, 
	    dopplerpos.fkdot[1], dopplerpos.fkdot[2], dopplerpos.fkdot[3] );
  }

  {
    int res = XLALadvanceLatticeIndex ( ret );
    if ( res < 0 ) {
      LALPrintError ( "\n\nXLALadvanceLatticeIndex() failed!\n\n");
      ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );    
    }
    else if ( res == 1 )
      {
	LALPrintError ( "\n\nXLALadvanceLatticeIndex(): no more lattice points!\n\n");
      }
      
    {
      PulsarDopplerParams dopplerpos;
      gsl_vector_int *index = NULL;
      if ( XLALgetCurrentDopplerPos ( &dopplerpos, ret, COORDINATESYSTEM_EQUATORIAL ) ) {
	ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );    
      }
      if ( XLALgetCurrentLatticeIndex ( &index, ret ) ) {
	ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );    
      }
      fprintf (stderr, "\nNew index = ");
      XLALfprintfGSLvector_int ( stderr, "%d", index );
      printf ("\nNew Doppler-position: {Freq, Alpha, Delta, f1dot, f2dot, f3dot} = {%f, %f, %f, %g, %g, %g}\n\n",
	      dopplerpos.fkdot[0], dopplerpos.Alpha, dopplerpos.Delta, 
	      dopplerpos.fkdot[1], dopplerpos.fkdot[2], dopplerpos.fkdot[3] );
    }
  }
  {
    int res = XLALadvanceLatticeIndex ( ret );
    if ( res < 0 ) {
      LALPrintError ( "\n\nXLALadvanceLatticeIndex() failed!\n\n");
      ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );    
    }
    else if ( res == 1 )
      {
	LALPrintError ( "\n\nXLALadvanceLatticeIndex(): no more lattice points!\n\n");
      }
    
    {
      PulsarDopplerParams dopplerpos;
      gsl_vector_int *index = NULL;
      if ( XLALgetCurrentDopplerPos ( &dopplerpos, ret, COORDINATESYSTEM_EQUATORIAL ) ) {
	ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );    
      }
      if ( XLALgetCurrentLatticeIndex ( &index, ret ) ) {
	ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );    
      }
      fprintf (stderr, "\nNew index = ");
      XLALfprintfGSLvector_int ( stderr, "%d", index );
      printf ("\nNew Doppler-position: {Freq, Alpha, Delta, f1dot, f2dot, f3dot} = {%f, %f, %f, %g, %g, %g}\n\n",
	      dopplerpos.fkdot[0], dopplerpos.Alpha, dopplerpos.Delta, 
	      dopplerpos.fkdot[1], dopplerpos.fkdot[2], dopplerpos.fkdot[3] );
    }
  }
  {
    int res = XLALadvanceLatticeIndex ( ret );
    if ( res < 0 ) {
      LALPrintError ( "\n\nXLALadvanceLatticeIndex() failed!\n\n");
      ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );    
    }
    else if ( res == 1 )
      {
	LALPrintError ( "\n\nXLALadvanceLatticeIndex(): no more lattice points!\n\n");
      }
    
    {
      PulsarDopplerParams dopplerpos;
      gsl_vector_int *index = NULL;
      if ( XLALgetCurrentDopplerPos ( &dopplerpos, ret, COORDINATESYSTEM_EQUATORIAL ) ) {
	ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );    
      }
      if ( XLALgetCurrentLatticeIndex ( &index, ret ) ) {
	ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );    
      }
      fprintf (stderr, "\nNew index = ");
      XLALfprintfGSLvector_int ( stderr, "%d", index );
      printf ("\nNew Doppler-position: {Freq, Alpha, Delta, f1dot, f2dot, f3dot} = {%f, %f, %f, %g, %g, %g}\n\n",
	      dopplerpos.fkdot[0], dopplerpos.Alpha, dopplerpos.Delta, 
	      dopplerpos.fkdot[1], dopplerpos.fkdot[2], dopplerpos.fkdot[3] );
    }
  }


#endif

  DETATCHSTATUSPTR ( status );
  RETURN ( status );

} /* InitDopplerLatticeScan() */


/** Free an allocated DopplerLatticeScan 
 */
void
FreeDopplerLatticeScan ( LALStatus *status, DopplerLatticeScan **scan )
{
  INITSTATUS( status, "FreeDopplerLatticeScan", DOPPLERLATTICECOVERING );

  ASSERT ( scan, status, DOPPLERSCANH_ENULL, DOPPLERSCANH_MSGENULL );  
  ASSERT ( *scan, status, DOPPLERSCANH_ENONULL, DOPPLERSCANH_MSGENONULL );  
  ASSERT ( (*scan)->state != STATE_IDLE, status, DOPPLERSCANH_EINPUT, DOPPLERSCANH_MSGEINPUT );

  if ( (*scan)->boundary.skyRegion.data )
    LALFree ( (*scan)->boundary.skyRegion.data );

  if ( (*scan)->latticeOrigin )
    gsl_vector_free ( (*scan)->latticeOrigin );

  if ( (*scan)->latticeGenerator )
    gsl_matrix_free ( (*scan)->latticeGenerator );

  if ( (*scan)->index )
    gsl_vector_int_free ( (*scan)->index );

  
  LALFree ( (*scan) );
  (*scan) = NULL;

  RETURN(status);

} /* FreeDopplerLatticeScan() */


/** Return current lattice index of the scan:
 * allocate index here if *index == NULL, otherwise use given vector 
 * [has to have right dimension] 
 */
int
XLALgetCurrentLatticeIndex ( gsl_vector_int **index, const DopplerLatticeScan *scan  )
{
  const CHAR *fn = "XLALgetCurrentLatticeIndex()";

  if ( !index || !scan || (scan->state != STATE_READY ) ) {
    XLAL_ERROR (fn, XLAL_EINVAL );
  }

  if ( *index == NULL )	/* allocate output-vector */
    {
      if ( ((*index) = gsl_vector_int_calloc ( scan->dimSearch )) == NULL ) {
	XLAL_ERROR (fn, XLAL_ENOMEM );
      }
    }
  else if ( (*index)->size != scan->dimSearch ) {
    LALPrintError ("\n\nOutput vector has wrong dimension %d instead of %d!\n\n", (*index)->size, scan->dimSearch);
    XLAL_ERROR (fn, XLAL_EINVAL );
  }
  
  gsl_vector_int_memcpy ( (*index), scan->index );  

  return 0;
} /* XLALgetCurrentLatticeIndex() */

/** Set the current index of the scan.
 */
int
XLALsetCurrentLatticeIndex ( DopplerLatticeScan *scan, const gsl_vector_int *index )
{
  const CHAR *fn = "XLALsetCurrentLatticeIndex()";

  if ( !index || !scan || (scan->state != STATE_READY) || (index->size != scan->dimSearch) ) {
    XLAL_ERROR (fn, XLAL_EINVAL );
  }

  gsl_vector_int_memcpy ( scan->index, index );

  return 0;
} /* XLALsetCurrentLatticeIndex() */


/** The central "lattice-stepping" function: advance to the 'next' index-point, taking care not
 * to leave the boundary, and to cover the whole (convex!) search-region eventually!
 *
 * \note Algorithm: 
 o) start with first index-dimension aI = 0
 1) if index(aI) >= 0: index(aI) ++; else   index(aI) --;
    i.e. increase for pos index, decrease for negative ones ==> always walk "outwards"
    from origin, towards boundaries)
 o) if resulting point lies inside boundary ==> keep & return

 o) if boundary was crossed: return aI to origin: index(aI) = 0;
 o) step to next dimension: aI ++;
 o) if no more dimensions left: aI >= dim ==> no further lattice-points! RETURN
 o) else continue at 1)
 *
 *
 * Return: 0=OK, -1=ERROR,  +1=No more lattice points left 
 */
int
XLALadvanceLatticeIndex ( DopplerLatticeScan *scan )
{
  const CHAR *fn = "XLALadvanceLatticeIndex()";
  UINT4 dim, aI;
  gsl_vector_int *index0 = NULL;
  gsl_vector_int *next_index, *index_step;
  int ret = 0;

  if ( !scan || scan->state != STATE_READY ) {
    XLAL_ERROR (fn, XLAL_EINVAL );
  }

  dim = scan->dimSearch;

  if ( XLALgetCurrentLatticeIndex ( &index0, scan ) ) {
    XLAL_ERROR (fn, XLAL_EFUNC );
  }

  next_index = gsl_vector_int_calloc ( dim );
  index_step = gsl_vector_int_calloc ( dim );

  aI = 0;
  do 
    {
      BOOLEAN goingUp = TRUE;

      gsl_vector_int_memcpy (next_index, index0);
      gsl_vector_int_set_basis (index_step, aI);	/* (0, 0, ..., 1, ..., 0, 0 ) */

      if ( gsl_vector_int_get ( index0, aI ) < 0 )
	goingUp = FALSE;	/* going down if negative index ==> always walk "out" from origin */

      if ( !goingUp )
	gsl_vector_int_scale (index_step, -1 );		/* change sign of step if negativ index */

      gsl_vector_int_add ( next_index, index_step );

      if ( isIndexInsideBoundary ( next_index, scan ) )
	{
	  if ( XLALsetCurrentLatticeIndex ( scan, next_index ) ) 
	    ret = -1;
	  else
	    ret = 0;		/* OK: we've advanced to the next valid point and we're done */
	  break;
	} /* index inside boundary */
      else
	{
	  if ( goingUp )	/* first try changing direction: re-start from index(aI) to '-1' */
	    {
	      gsl_vector_int_memcpy (next_index, index0);	/* return to initial index */
	      gsl_vector_int_set ( next_index, aI, -1 );
	      if ( isIndexInsideBoundary ( next_index, scan ) )
		{
		  if ( XLALsetCurrentLatticeIndex ( scan, next_index ) ) 
		    ret = -1;
		  else
		    ret = 0;		/* OK: we've advanced to the next valid point and we're done */
		  break;
		}
	    } /* going up */

	  /* tried going down already => return index(aI)=0 and move to next dimension */
	  gsl_vector_int_set ( index0, aI, 0 );
	  aI ++;
	} /* index outside boundary */
      
    } while ( aI < dim );
	  
  if ( aI == dim )	/* no further lattice point was found inside the boundary ==> we're done */
    ret = 1;

  gsl_vector_int_free ( index0 );
  gsl_vector_int_free ( next_index );
  gsl_vector_int_free ( index_step );
  
  if ( ret < 0 ) {
    XLAL_ERROR (fn, XLAL_EFUNC );
  }
  else
    return ret;

} /* XLALadvanceLatticeIndex() */



/** Return the current doppler-position {Freq, Alpha, Delta, f1dot, f2dot, ... } of the lattice-scan
 *  NOTE: the skyposition coordinate-system is chosen via 'skyCoords' in [EQUATORIAL, ECLIPTIC] 
 */
int
XLALgetCurrentDopplerPos ( PulsarDopplerParams *pos, const DopplerLatticeScan *scan, CoordinateSystem skyCoords )
{
  const CHAR *fn = "XLALgetCurrentDopplerPos()";
  dopplerParams_t doppler = empty_dopplerParams;
  SkyPosition skypos = empty_SkyPosition;

  if ( !pos || !scan || (scan->state != STATE_READY) ) {
    XLAL_ERROR (fn, XLAL_EINVAL );
  }

  if ( XLALindexToDoppler ( &doppler, scan->index, scan ) ) {
    XLAL_ERROR (fn, XLAL_EFUNC );
  }

  skypos.system = skyCoords;
  if ( vect3DToSkypos ( &skypos, (const vect3D_t*)&doppler.vn ) ) {
    XLAL_ERROR (fn, XLAL_EFUNC );
  }

  /* convert into PulsarDopplerParams type */
  pos->refTime 		= scan->boundary.fkRange.refTime;
  pos->Alpha		= skypos.longitude;
  pos->Delta		= skypos.latitude;
  memcpy ( pos->fkdot, doppler.fkdot, sizeof(pos->fkdot) );
  pos->orbit 		= NULL;		/* FIXME: not supported yet */

  return 0;
} /* XLALgetCurrentDopplerPos() */



/* ------------------------------------------------------------ */
/* -------------------- INTERNAL functions -------------------- */
/* ------------------------------------------------------------ */


/** Convert given index into doppler-params
 * Return: 0=OK, -1=ERROR
 */
int
XLALindexToDoppler ( dopplerParams_t *doppler, const gsl_vector_int *index, const DopplerLatticeScan *scan )
{
  const CHAR *fn = "XLALindexToDoppler()";
  gsl_vector *canonical = NULL;  

  if ( !doppler || !index || !scan ) {
    XLAL_ERROR (fn, XLAL_EINVAL );
  }

  if ( (canonical = gsl_vector_calloc ( scan->latticeOrigin->size )) == NULL ) {
    XLAL_ERROR (fn, XLAL_ENOMEM );
  }

  if ( indexToCanonicalOffset ( &canonical, index, scan->latticeGenerator ) ) {
    gsl_vector_free ( canonical );
    XLAL_ERROR (fn, XLAL_EFUNC );
  }

  if ( gsl_vector_add (canonical, scan->latticeOrigin ) ) {
    gsl_vector_free ( canonical );
    XLAL_ERROR (fn, XLAL_EFUNC );
  }

  if ( convertCanonical2Doppler ( doppler, canonical, scan->boundary.hemisphere, scan->Tspan ) ) {
    gsl_vector_free ( canonical );
    XLAL_ERROR (fn, XLAL_EFUNC );
  }
  gsl_vector_free ( canonical );

  return 0;

} /* XLALindexToDoppler() */



/** Determine whether the given lattice-index corresponds to a Doppler-point 
 * that lies within the search-boundary 
 *  Return: TRUE, FALSE,  -1 = ERROR
 */
int
isIndexInsideBoundary ( const gsl_vector_int *index, const DopplerLatticeScan *scan )
{
  dopplerParams_t doppler = empty_dopplerParams;
  
  if ( !index || !scan || scan->state != STATE_READY )
    return -1;

  if ( XLALindexToDoppler ( &doppler, index, scan ) )
    return -1;

  return ( isDopplerInsideBoundary ( &doppler,  &scan->boundary ) );
  
} /* isIndexInsideBoundary() */


/** Determine whether the given Doppler-point lies within the search-boundary 
 *  Return: TRUE, FALSE,  -1 = ERROR
 */
int
isDopplerInsideBoundary ( const dopplerParams_t *doppler,  const dopplerBoundary_t *boundary )
{
  vect2D_t skyPoint = {0, 0};
  int insideSky, insideSpins, sameHemi;
  hemisphere_t this_hemi;
  UINT4 numSpins;
  UINT4 s;

  if ( !doppler || !boundary )
    return -1;

  numSpins = sizeof(doppler->fkdot) / sizeof(doppler->fkdot[0]);

  skyPoint[0] = doppler->vn[0];
  skyPoint[1] = doppler->vn[1];
  this_hemi = VECT_HEMI ( doppler->vn );

  if ( (insideSky = vect2DInPolygon ( (const vect2D_t*)skyPoint, &(boundary->skyRegion) )) < 0 )
    return -1;

  if ( this_hemi != boundary->hemisphere )
    sameHemi = FALSE;
  else
    sameHemi = TRUE;

  insideSpins = TRUE;
  for ( s=0; s < numSpins; s ++ )
    {
      double fkdotMax = boundary->fkRange.fkdot[s] + boundary->fkRange.fkdotBand[s];
      double fkdotMin = boundary->fkRange.fkdot[s];
      if ( ( gsl_fcmp ( doppler->fkdot[s], fkdotMax, EPS_REAL8 ) > 0 ) ||
	   ( gsl_fcmp ( doppler->fkdot[s], fkdotMin, EPS_REAL8 ) < 0 ) )
	insideSpins = ( insideSpins && FALSE );
    } /* for s < numSpins */
  
  return ( insideSky && sameHemi && insideSpins );

} /* insideBoundary() */


/** Translate the input 'DopplerRegion' into an internal representation for the scan.
 * 
 * \note DopplerLatticeScan->Tspan must have been set! This is used for conversion Doppler -> canonical 
 */
void
setupSearchRegion ( LALStatus *status, DopplerLatticeScan *scan, const DopplerRegion *searchRegion )
{
  UINT4 i;
  vect3Dlist_t *points3D = NULL;
  dopplerParams_t midPoint = empty_dopplerParams;
  UINT4 numSpins = sizeof(PulsarSpins) / sizeof(REAL8) ;	/* number of elements in PulsarSpin array */

  INITSTATUS( status, "InitDopplerLatticeScan", DOPPLERLATTICECOVERING );
  ATTATCHSTATUSPTR ( status );

  ASSERT ( scan, status, DOPPLERSCANH_ENULL, DOPPLERSCANH_MSGENULL );  
  ASSERT ( searchRegion, status, DOPPLERSCANH_ENULL, DOPPLERSCANH_MSGENULL );  

  /* ----- sky ----- */
  TRY ( skyRegionString2vect3D ( status->statusPtr, &points3D, searchRegion->skyRegionString ), status );

  if ( (scan->boundary.hemisphere = onWhichHemisphere ( points3D )) == HEMI_BOTH ) 
    {
      LALFree ( points3D->data ); LALFree ( points3D );
      LALPrintError ("\n\nSorry, currently only (ecliptic) single-hemisphere sky-regions are supported!\n");
      ABORT ( status, DOPPLERSCANH_EINPUT, DOPPLERSCANH_MSGEINPUT );
    }
  if ( (scan->boundary.skyRegion.data = LALCalloc ( points3D->length, sizeof(scan->boundary.skyRegion.data[0]) )) == NULL ) 
    {
      LALFree ( points3D->data ); LALFree ( points3D );
      ABORT (status, DOPPLERSCANH_EMEM, DOPPLERSCANH_MSGEMEM);
    }
  scan->boundary.skyRegion.length = points3D->length;

  for ( i=0; i < points3D->length; i ++ ) {
    scan->boundary.skyRegion.data[i][0] = points3D->data[i][0];
    scan->boundary.skyRegion.data[i][1] = points3D->data[i][1];
  }

  findCenterOfMass ( &(midPoint.vn), points3D );

  LALFree ( points3D->data );
  LALFree ( points3D );

  /* ----- spins ----- */
  scan->boundary.fkRange.refTime   = searchRegion->refTime;
  memcpy ( scan->boundary.fkRange.fkdot, searchRegion->fkdot, sizeof(searchRegion->fkdot) );
  memcpy ( scan->boundary.fkRange.fkdotBand, searchRegion->fkdotBand, sizeof(searchRegion->fkdotBand) );

  for ( i=0; i < numSpins; i ++ )
    midPoint.fkdot[i] = searchRegion->fkdot[i] + 0.5 * searchRegion->fkdotBand[i];

  /* ----- use the center of the searchRegion as origin of the lattice ----- */
  if ( 0 != convertDoppler2Canonical ( &(scan->latticeOrigin), &midPoint, scan->Tspan ) ) {
    LALPrintError ("\n\nconvertDoppler2Canonical() failed!\n");
    ABORT ( status, DOPPLERSCANH_EXLAL, DOPPLERSCANH_MSGEXLAL );
  }

  /* determine number of spins to compute metric for (at least 1) */
  while ( (numSpins > 1) && (searchRegion->fkdotBand[numSpins - 1] == 0) )
    numSpins --;

  scan->dimSearch = 2 + numSpins;	/* sky + spins (must be at least 3) */

  DETATCHSTATUSPTR ( status );
  RETURN ( status );

} /* setupSearchRegion() */

/** Convert index-vector {i0, i1, i2, ..} into canonical offset Delta{w0, kX, kY, w1, w2, ...}
 * 
 * \note the output-vector 'canonicalOffset' can be NULL, in which case it will be allocated here,
 * with dimension == dim(index-vector). If allocated already, its dimension must be >= dim(index)
 * 
 * Return: 0=OK, -1=ERROR
 */
int 
indexToCanonicalOffset ( gsl_vector **canonicalOffset, const gsl_vector_int *index, const gsl_matrix *generator )
{
  UINT4 dim, i, j;

  /* check input */  
  if ( !canonicalOffset || !index || !generator)
    return -1;

  dim = index->size;
  if ( ( generator->size1 != dim ) || ( generator->size2 != dim ) ) {
    LALPrintError ("\nindexToCanonicalOffset(): generator has to be square-matrix of same dimension as index-vector!\n\n");
    return -1;
  }
  if ( (*canonicalOffset != NULL) && ( (*canonicalOffset)->size < dim ) ) {
    LALPrintError ("\nindexToCanonicalOffset(): output-vector has to have AT LEAST same dimension as index-vector!\n\n");
    return -1;
  }

  /* allocate or initialize output-vector */
  if ( (*canonicalOffset) == NULL )
    {
      if ( ((*canonicalOffset) = gsl_vector_calloc ( dim )) == NULL )
	return -1;
    }
  else
    gsl_vector_set_zero ( *canonicalOffset );

  /* lattice-vect = index . generator */
  for ( i=0; i < dim; i ++ )	
    {
      REAL8 comp = 0;
      for (j=0; j < dim; j ++ )
	comp += 1.0 * gsl_vector_int_get ( index, j ) * gsl_matrix_get ( generator, j, i );

      gsl_vector_set ( (*canonicalOffset), i, comp );

    } /* i < dim */

  return 0;

} /* indexToCanonicalOffset() */


/** Convert Doppler-parameters from {nX, nY, nZ, fkdot} into internal 'canonical' form
 *  {w0, kX, kY, w1, w1, ... } 
 * \note The return-vector is allocated here and needs to be gsl_vector_free()'ed
 * 
 * Return: 0=OK, -1=ERROR
 */
int
convertDoppler2Canonical ( gsl_vector **canonicalPoint, const dopplerParams_t *doppler, REAL8 Tspan )
{
  REAL8 kX, kY;
  REAL8 prefix;
  UINT4 s;
  PulsarSpins wk;
  gsl_vector *ret;
  UINT4 numSpins = sizeof(wk) / sizeof(wk[0]);

  if ( !canonicalPoint || (*canonicalPoint != NULL) || !doppler )
    return -1;

  prefix = (LAL_TWOPI * LAL_AU_SI / LAL_C_SI ) * doppler->fkdot[0];
  kX = -prefix * doppler->vn[0];		/* vk = - 2*pi * Rorb/c * Freq * vn */
  kY = -prefix * doppler->vn[1];
  
  convertSpins2Canonical ( wk, doppler->fkdot, Tspan );
  
  if ( (ret = gsl_vector_calloc ( 2 + numSpins )) == NULL ) {
    LALPrintError("\n\nOut of memory!!\n");
    return -1;
  }
  gsl_vector_set (ret, 0, wk[0]);
  gsl_vector_set (ret, 1, kX );
  gsl_vector_set (ret, 2, kY );
  for ( s=1; s < numSpins; s ++ )
    gsl_vector_set (ret, 2 + s, wk[s] );

  /* return vector */
  (*canonicalPoint) = ret;

  return 0;

} /* convertDoppler2Canonical() */

/** Convert SI-unit spins 'fkdot' into canonical units w^(s) = 2*pi * f^(s) * T^(s+1) 
 */
int
convertSpins2Canonical ( PulsarSpins wk, const PulsarSpins fkdot, REAL8 Tspan )
{
  PulsarSpins dummy;
  UINT4 numSpins = sizeof(dummy) / sizeof(dummy[0]);
  REAL8 prefact = LAL_TWOPI * Tspan;
  UINT4 s;

  for ( s=0; s < numSpins; s ++ )
    {
      wk[s] = prefact * fkdot[s];	/* wk = 2*pi * T^(s+1) * fkdot */
      prefact *= Tspan;
    }

  return 0;

} /* convertSpins2Canonical() */

/** Convert a 'canonical' parameter-space point back into a 'physical' Doppler units: 
 * a unit (ecliptic) sky-vector 'vn' and spin-vector 'fkdot' 
 * 
 * Return: 0=OK, -1=ERROR
 */
int 
convertCanonical2Doppler ( dopplerParams_t *doppler, const gsl_vector *canonical, hemisphere_t hemi, REAL8 Tspan )
{
  REAL8 prefact, vn2;
  UINT4 numSpins, numSpinsMax, s;

  if ( !canonical || !doppler )
    return -1;

  if ( (hemi != HEMI_NORTH) && ( hemi != HEMI_SOUTH ) ) {
    LALPrintError ("Need to choose either HEMI_NORTH or HEMI_SOUTH!\n");
    return -1;
  }

  numSpinsMax = sizeof(doppler->fkdot) / sizeof(doppler->fkdot[0]);
  for (s=0; s < numSpinsMax; s ++ )
    doppler->fkdot[s] = 0;	/* init to zero */

  numSpins = canonical->size - 2;

  if ( numSpins > numSpinsMax ) {
    LALPrintError ("\n\nERROR: Canonical Point has more spins (%d) than maximum allowed (%d)!\n", numSpins, numSpinsMax );
    return -1;
  }

  /* spins */
  prefact = LAL_TWOPI * Tspan;
  doppler->fkdot[0] = gsl_vector_get ( canonical, 0 ) / prefact;	/* w0 / (2*pi*T) */
  for ( s=1; s < numSpins; s ++ )
    {
      prefact *= Tspan;
      doppler->fkdot[s] = gsl_vector_get ( canonical, s+2 ) / prefact;	/* wk / (2*pi*T^(k+1)) */
    }
  
  /* sky */
  prefact = (LAL_TWOPI * LAL_AU_SI / LAL_C_SI) * doppler->fkdot[0];

  doppler->vn[0] = - gsl_vector_get ( canonical, 1 ) / prefact;	/* nX = - kX / ( 2*pi*Rorb*f/c ) */
  doppler->vn[1] = - gsl_vector_get ( canonical, 2 ) / prefact;	/* nY = - kY / ( 2*pi*Rorb*f/c ) */

  vn2 = SQUARE ( doppler->vn[0] ) + SQUARE ( doppler->vn[1] );
  if ( gsl_fcmp ( vn2, 1.0, EPS_REAL8 ) > 0 ) {
    LALPrintError ( "\n\nconvertCanonical2Doppler(): Sky-vector has length > 1! (vn2 = %f)\n\n", vn2 );
    return -1;
  }
  
  doppler->vn[2] = sqrt ( fabs ( 1.0 - vn2 ) );		/* nZ = sqrt(1 - nX^2 - nY^2 ) */

  if ( hemi == HEMI_SOUTH )
    doppler->vn[2] *= -1;

  return 0;

} /* convertCanonical2Doppler() */


/** Convert a sky-region string into a list of vectors in ecliptic 2D coords {nX, nY} 
 */
void
skyRegionString2vect3D ( LALStatus *status, 
			 vect3Dlist_t **skyRegion, 	/**< [out] list of skypoints in 3D-ecliptic coordinates {nX, nY, nZ} */
			 const CHAR *skyRegionString )	/**< [in] string of equatorial-coord. sky-positions */
{
  vect3Dlist_t *ret = NULL;
  SkyRegion region = empty_SkyRegion;
  UINT4 i;

  INITSTATUS( status, "skyRegionString2list", DOPPLERLATTICECOVERING );
  ATTATCHSTATUSPTR ( status );

  ASSERT ( skyRegion, status, DOPPLERSCANH_ENULL, DOPPLERSCANH_MSGENULL );  
  ASSERT ( *skyRegion == NULL, status, DOPPLERSCANH_ENONULL, DOPPLERSCANH_MSGENONULL );  
  ASSERT ( skyRegionString, status, DOPPLERSCANH_ENULL, DOPPLERSCANH_MSGENULL );  

  TRY ( ParseSkyRegionString (status->statusPtr, &region, skyRegionString), status );

  if ( ( ret = LALCalloc ( 1, sizeof(*ret)) ) == NULL ) {
    ABORT (status, DOPPLERSCANH_EMEM, DOPPLERSCANH_MSGEMEM);    
  }
  if ( (ret->data = LALCalloc ( region.numVertices, sizeof( ret->data[0] ) )) == NULL ) {
    LALFree ( ret );
    ABORT (status, DOPPLERSCANH_EMEM, DOPPLERSCANH_MSGEMEM);    
  }
  ret->length = region.numVertices;
  
  for ( i=0; i < region.numVertices; i ++ )
    skyposToVect3D ( &(ret->data[i]), &(region.vertices[i]) );

  /* return sky-region */
  (*skyRegion) = ret;

  DETATCHSTATUSPTR ( status );
  RETURN ( status );

} /* skyRegionString2list() */

/** Check whether given list of skypoint lie on a single hemisphere 
 */
hemisphere_t
onWhichHemisphere ( const vect3Dlist_t *skypoints )
{
  UINT4 i;
  hemisphere_t our_hemi = HEMI_BOTH;

  if ( !skypoints || (skypoints->length == 0) )
    return FALSE;

  for ( i=0; i < skypoints->length; i ++ )
    {
      vect3D_t *thisPoint = &(skypoints->data[i]);
      hemisphere_t this_hemi = VECT_HEMI ( *thisPoint );
      if ( (our_hemi == HEMI_BOTH) && (this_hemi != HEMI_BOTH) ) /* set our_hemi to first non-zero hemisphere */
	our_hemi = this_hemi;
      if ( (this_hemi != HEMI_BOTH) && (this_hemi != our_hemi ) )
	return HEMI_BOTH;
    }
  
  return our_hemi;
  
} /* onWhichHemisphere() */

/** Convert a 'SkyPosition' {alpha, delta} into a 3D unit vector in ecliptic coordinates 
 * Return: 0=OK, -1=ERROR
 */
int 
skyposToVect3D ( vect3D_t *eclVect, const SkyPosition *skypos )
{
  REAL8 cosa, cosd, sina, sind, sineps, coseps;
  REAL8 nn[3];	/* unit-vector pointing to source */

  if ( !eclVect || !skypos )
    return -1;

  sina = sin(skypos->longitude);
  cosa = cos(skypos->longitude);
  sind = sin(skypos->latitude);
  cosd = cos(skypos->latitude);

  nn[0] = cosa * cosd;
  nn[1] = sina * cosd;
  nn[2] = sind;

  switch ( skypos->system )
    {
    case COORDINATESYSTEM_EQUATORIAL:
      sineps = SIN_EPS; coseps = COS_EPS;
      break;
    case COORDINATESYSTEM_ECLIPTIC:
      sineps = 0; coseps = 1;
      break;
    default:
      XLAL_ERROR ( "skyposToVect3D", XLAL_EINVAL );
      break;
    } /* switch(system) */

  (*eclVect)[0] =   nn[0];
  (*eclVect)[1] =   nn[1] * coseps + nn[2] * sineps;
  (*eclVect)[2] = - nn[1] * sineps + nn[2] * coseps;

  return 0;

} /* skyposToVect3D() */

/** Convert (ecliptic) vector pointing to a skypos back into (alpha, delta) 
 *
 * NOTE: The coordinate-system of the result is determined by the 'system'-setting in
 * the output 'skypos'
 * 
 * return: 0=OK, -1=ERROR
 */
int
vect3DToSkypos ( SkyPosition *skypos, const vect3D_t *vect )
{
  REAL8 invnorm;
  vect3D_t nvect = {0,0,0};
  REAL8 longitude, latitude;
  REAL8 sineps, coseps;

  if ( !skypos || !vect )
    return -1;

  switch ( skypos->system )
    {
    case COORDINATESYSTEM_EQUATORIAL:
      sineps = SIN_EPS; coseps = COS_EPS;
      break;
    case COORDINATESYSTEM_ECLIPTIC:
      sineps = 0; coseps = 1;
      break;
    default:
      XLAL_ERROR ( "vect3DToSkypos", XLAL_EINVAL );
      break;
    } /* switch(system) */

  nvect[0] = (*vect)[0];
  nvect[1] = coseps * (*vect)[1] - sineps * (*vect)[2];
  nvect[2] = sineps * (*vect)[1] + coseps * (*vect)[2];

  invnorm = 1.0 / VECT_NORM ( nvect );
  
  VECT_MULT ( nvect, invnorm );

  longitude = atan2 ( nvect[1], nvect[0]);
  if ( longitude < 0 )
    longitude += LAL_TWOPI;

  latitude = asin ( nvect[2] );

  skypos->longitude = longitude;
  skypos->latitude = latitude;

  return 0;

} /* vect3DToSkypos() */


/** Find the "center of mass" of the given list of 3D points
 * Return: 0=OK, -1 on ERROR
 */
int
findCenterOfMass ( vect3D_t *center, const vect3Dlist_t *points )
{
  UINT4 i;
  vect3D_t com = {0, 0, 0};	/* center of mass */
  REAL8 norm;

  if ( !center || !points )
    return -1;

  for ( i=0; i < points->length ; i ++ )
    {
      VECT_ADD ( com, points->data[i] );
    }
  norm = 1.0 / points->length;
  VECT_MULT ( com, norm );

  VECT_COPY ( (*center), com );

  return 0;

} /* findCenterOfmass() */


/** Function for checking if a given vect2D-point lies inside or outside a given vect2D-polygon.
 * This is basically indentical to 'pointInPolygon()' only using different data-types.
 *
 * \par Note1: 
 * 	The list of polygon-points must not close on itself, the last point
 * 	is automatically assumed to be connected to the first 
 * 
 * \par Alorithm: 
 *     Count the number of intersections of rays emanating to the right
 *     from the point with the lines of the polygon: even=> outside, odd=> inside
 *
 * \par Note2: 
 *     we try to get this algorith to count all boundary-points as 'inside'
 *     we do this by counting intersection to the left _AND_ to the right
 *     and consider the point inside if either of those says its inside...
 *
 * \par Note3: 
 *     correctly handles the case of a 1-point 'polygon', in which the two
 *     points must agree within eps=1e-10 relative precision.
 * 
 * \return : TRUE or FALSE, -1=ERROR
 *----------------------------------------------------------------------*/
int
vect2DInPolygon ( const vect2D_t *point, const vect2Dlist_t *polygon )
{
  UINT4 i;
  UINT4 N;
  UINT4 insideLeft, insideRight;
  BOOLEAN inside = 0;
  vect2D_t *vertex;
  REAL8 xinter, v1x, v1y, v2x, v2y, px, py;

  if (!point || !polygon || !polygon->data )
    return -1;

  /* convenience variables */
  vertex = polygon->data;
  N = polygon->length; 	/* num of vertices = num of edges */
  px = (*point)[0];
  py = (*point)[1];

  /* treat special case of 1-point 'polygon' ==> float-number comparisons */
  if ( N == 1 )
    {
      int diffx, diffy;
      double eps = 1e-10;	/* allow relative errors of up to 1e-10 */
      diffx = gsl_fcmp ( vertex[0][0], px, eps );
      diffy = gsl_fcmp ( vertex[0][1], py, eps );
      return ( (diffx == 0) && (diffy == 0) );
    }
  else if ( N < 3 )	/* need 3 points for an area */
    return -1;

  /* general case of 2D-polygon */

  insideLeft = insideRight = 0;

  for (i=0; i < N; i++)
    {
      v1x = vertex[i][0];
      v1y = vertex[i][1];
      v2x = vertex[(i+1) % N][0];
      v2y = vertex[(i+1) % N][1];

      /* pre-select candidate edges */
      if ( (py <  MIN(v1y,  v2y)) || (py >=  MAX(v1y, v2y) ) || (v1y == v2y) )
	continue;

      /* now calculate the actual intersection point of the horizontal ray with the edge in question*/
      xinter = v1x + (py - v1y) * (v2x - v1x) / (v2y - v1y);

      if (xinter > px)	      /* intersection lies to the right of point */
	insideLeft ++;

      if (xinter < px)       /* intersection lies to the left of point */
	insideRight ++;

    } /* for sides of polygon */

  inside = ( ((insideLeft %2) == 1) || (insideRight %2) == 1);

  return inside;
  
} /* vect2DInPolygon() */
