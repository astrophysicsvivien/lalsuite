/*
*  Copyright (C) 2007 Jolien Creighton
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

#ifndef INVSPEC_H
#define INVSPEC_H

/*
 *
 * Routine to compute the average spectrum from time series data.
 * Routine to invert and truncate (to have compact time support) a spectrum.
 * Routine to scale a spectrum by the magnitude of the response function.
 *
 */

#include <lal/LALDatatypes.h>


/* routine to compute an average spectrum from time series data */
REAL4FrequencySeries *compute_average_spectrum(
    REAL4TimeSeries         *series,
    int                      spectrumAlgthm,
    REAL8                    segmentDuration,
    REAL8                    strideDuration,
    REAL4FFTPlan            *fwdplan,
    int                      whiteSpectrum
    );


/* Routine to generate a theoretical PSD */
REAL4FrequencySeries *generate_theoretical_psd(
    REAL4                    deltaT,
    REAL8                    segmentDuration,
    UINT4                    spectrumNumber
    );


/* routine to invert and truncate (to have compact time support) a spectrum */
int invert_spectrum(
    REAL4FrequencySeries *spectrum,
    REAL8                 dataSampleRate,
    REAL8                 strideDuration,
    REAL8                 truncateDuration,
    REAL8                 lowCutoffFrequency,
    REAL4FFTPlan         *fwdplan,
    REAL4FFTPlan         *revplan
    );


/* routine to scale a spectrum by the magnitude of the response function */
int calibrate_spectrum(
    REAL4FrequencySeries    *spectrum,
    COMPLEX8FrequencySeries *response,
    REAL8                    lowCutoffFrequency,
    int                      inverse
    );

typedef enum
{
  WHITE_PSD,
  ILIGO_PSD
}
spectrumType;

#endif /* INVSPEC_H */
