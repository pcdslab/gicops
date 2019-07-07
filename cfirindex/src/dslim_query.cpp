/*
 * This file is part of Load Balancing Algorithm for DSLIM
 *  Copyright (C) 2019  Muhammad Haseeb, Fahad Saeed
 *  Florida International University, Miami, FL
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "dslim.h"
#include "hyperscore.h"

extern gParams   params;
extern BYICount  *Score;

#ifdef BENCHMARK
static DOUBLE duration = 0;
extern DOUBLE compute;
extern DOUBLE fileio;
extern DOUBLE memory;
#endif /* BENCHMARK */

/* Global variables */
INT qspecnum = -1;
INT chnum    = -1;


#ifdef FUTURE
extern pepEntry    *pepEntries; /* SLM Peptide Index */
extern PepSeqs          seqPep; /* Peptide sequences */
#ifdef VMODS
extern varEntry    *modEntries;
#endif /* VMODS */
#endif /* FUTURE */

/* Global Variables */
FLOAT *hyperscores = NULL;
UCHAR *sCArr = NULL;

/* FUNCTION: DSLIM_QuerySpectrum
 *
 * DESCRIPTION: Query the DSLIM for all query peaks
 *              and count the number of hits per chunk
 *
 * INPUT:
 * @QA     : Query Spectra Array
 * @len    : Number of spectra in the array
 * @Matches: Array to fill in the number of hits per chunk
 * @threads: Number of parallel threads to launch
 *
 * OUTPUT:
 * @status: Status of execution
 */
STATUS DSLIM_QuerySpectrum(ESpecSeqs &ss, UINT len, Index *index, UINT idxchunk)
{
    STATUS status = SLM_SUCCESS;
    UINT maxz = params.maxz;
    UINT dF = params.dF;
    UINT threads = params.threads;
    UINT scale = params.scale;
    DOUBLE maxmass = params.max_mass;

    if (Score == NULL)
    {
        status = ERR_INVLD_MEMORY;
    }

    if (status == SLM_SUCCESS)
    {
        status = HS_InitFile();
    }

#ifdef BENCHMARK
    DOUBLE tcons[threads];
    std::memset(tcons, 0x0, threads * sizeof(DOUBLE));
#endif /* BENCHMARK */

#ifndef _OPENMP
    LBE_UNUSED_PARAM(threads);
#endif /* _OPENMP */

#ifdef BENCHMARK
    duration = omp_get_wtime();
#endif /* BENCHMARK */

    if (status == SLM_SUCCESS)
    {
        /* Process all the queries in the chunk */
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(dynamic, 1)
#endif /* _OPENMP */
         for (UINT queries = 0; queries < len; queries++)
         {
#ifdef BENCHMARK
            DOUBLE stime = omp_get_wtime();
#endif
             /* Pointer to each query spectrum */
             UINT *QAPtr = ss.moz + ss.idx[queries];
             FLOAT *iPtr = ss.intensity + ss.idx[queries];
             UINT qspeclen = ss.idx[queries + 1] - ss.idx[queries];
             UINT thno = omp_get_thread_num();

             UCHAR *bcPtr =  Score[thno].bc;
             UCHAR *ycPtr =  Score[thno].yc;
             FLOAT *ibcPtr = Score[thno].ibc;
             FLOAT *iycPtr = Score[thno].iyc;

            if (thno == 0 && !(queries % 50))
            {
                std::cout << '\r' << "DONE: " << (queries * 100) /len << "%";
            }

            for (UINT ixx = 0; ixx < idxchunk; ixx++)
            {
                UINT speclen = (index[ixx].pepIndex.peplen - 1) * maxz * iSERIES;

                for (UINT chno = 0; chno < index[ixx].nChunks; chno++)
                {
                    /* Query each chunk in parallel */
                    UINT *bAPtr = index[ixx].ionIndex[chno].bA;
                    UINT *iAPtr = index[ixx].ionIndex[chno].iA;

                    /* Query all fragments in each spectrum */
                    for (UINT k = 0; k < qspeclen; k++)
                    {
                        /* Check for any zeros
                         * Zero = Trivial query */
                        if (QAPtr[k] > dF && QAPtr[k] < ((maxmass * scale) - 1 - dF))
                        {
                            /* Locate iAPtr start and end */
                            UINT start = bAPtr[QAPtr[k] - dF];
                            UINT end = bAPtr[QAPtr[k] + 1 + dF];

                            /* Loop through located iAions */
                            for (UINT ion = start; ion < end; ion++)
                            {
                                UINT raw = iAPtr[ion];

                                /* Calculate parent peptide ID */
                                UINT ppid = (raw / speclen);

                                /* Update corresponding scorecard entries */
                                if ((raw % speclen) < speclen / 2)
                                {
                                    bcPtr[ppid] += 1;
                                    ibcPtr[ppid] += iPtr[k];
                                }
                                else
                                {
                                    ycPtr[ppid] += 1;
                                    iycPtr[ppid] += iPtr[k];
                                }
                            }
                        }
                    }

                    Score[thno].especid = queries;

                    INT idaa = -1;
                    FLOAT maxhv = 0.0;

                    UINT csize = ((chno == index[ixx].nChunks - 1) && (index[ixx].nChunks > 1)) ?
                                    index[ixx].lastchunksize : index[ixx].chunksize;

                    for (UINT it = 0; it < csize; it++)
                    {
                        if (bcPtr[it] + ycPtr[it] > params.min_shp)
                        {
                            FLOAT hyperscore = log(HYPERSCORE_Factorial(ULONGLONG(bcPtr[it])) *
                                                   HYPERSCORE_Factorial(ULONGLONG(ycPtr[it])) *
                                                   ibcPtr[it] *
                                                   iycPtr[it]);

                            if (hyperscore > maxhv)
                            {
                                idaa = DSLIM_GenerateIndex(&index[ixx], it);
                                maxhv = hyperscore;
                            }
                        }

                        /* Clear the scorecard as you read */
                        bcPtr[it] = 0;
                        ycPtr[it] = 0;
                        ibcPtr[it] = 0;
                        iycPtr[it] = 0;
                    }

                    /* Print the highest hyperscore per chunk */
#pragma omp critical
                    {
                        /* Printing the hyperscore in OpenMP mode */
                        status = HYPERSCORE_Calculate(queries, idaa, maxhv);
                    }
                }
            }

#ifdef BENCHMARK
            tcons[thno] += omp_get_wtime() - stime;
#endif
        }
    }

    std::cout << '\n';

#ifdef BENCHMARK
    compute += omp_get_wtime() - duration;

    for (unsigned int thd = 0; thd < params.threads; thd++)
    {
        std::cout << "Thread #: " << thd << "\t" << tcons[thd] << std::endl;
    }
#endif

    return status;
}

STATUS DSLIM_DeallocateSC(VOID)
{
    /* Free the Scorecard memory */
    if (Score != NULL)
    {
        for (UINT thd = 0; thd < params.threads; thd++)
        {
            delete[] Score[thd].bc;
            delete[] Score[thd].ibc;
            delete[] Score[thd].yc;
            delete[] Score[thd].iyc;

            Score[thd].bc = NULL;
            Score[thd].ibc = NULL;
            Score[thd].yc = NULL;
            Score[thd].iyc = NULL;

            Score[thd].especid = 0;
            Score[thd].size = 0;
        }

        delete[] Score;
        Score = NULL;

    }

    return SLM_SUCCESS;
}
