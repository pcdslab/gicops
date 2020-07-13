/*
 * Copyright (C) 2019  Muhammad Haseeb, and Fahad Saeed
 * Florida International University, Miami, FL
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include "dslim_fileout.h"
#include "msquery.h"
#include "dslim.h"
#include "lwqueue.h"
#include "lwbuff.h"
#include "scheduler.h"

using namespace std;

extern gParams   params;
extern BYICount  *Score;
extern vector<STRING> queryfiles;

/* Global variables */
FLOAT *hyperscores         = NULL;
UCHAR *sCArr               = NULL;
BOOL   ExitSignal          = false;

#ifdef DISTMEM
DSLIM_Comm *CommHandle    = NULL;
hCell      *CandidatePSMS  = NULL;
#endif /* DISTMEM */

Scheduler  *SchedHandle    = NULL;
expeRT     *ePtrs          = NULL;

ebuffer    *iBuff          = NULL;
INT         ciBuff         = -1;
LOCK       writer;

/* Lock for query file vector and thread manager */
LOCK qfilelock;
lwqueue<MSQuery *> *qfPtrs = NULL;
MSQuery **ptrs             = NULL;
INT spectrumID             = 0;
INT nBatches               = 0;
INT dssize                 = 0;

/* Expt spectra data buffer */
lwbuff<Queries> *qPtrs     = NULL;
Queries *workPtr           = NULL;

/****************************************************************/

#ifdef DISTMEM
VOID *DSLIM_FOut_Thread_Entry(VOID *argv);
#endif

/* A queue containing I/O thread state when preempted */
lwqueue<MSQuery *> *ioQ = NULL;
LOCK ioQlock;
/****************************************************************/

#ifdef BENCHMARK
static DOUBLE duration = 0;
extern DOUBLE compute;
extern DOUBLE fileio;
extern DOUBLE memory;
#endif /* BENCHMARK */

VOID *DSLIM_IO_Threads_Entry(VOID *argv);

/* Static function */
static BOOL DSLIM_BinarySearch(Index *, FLOAT, INT&, INT&);
static INT  DSLIM_BinFindMin(pepEntry *entries, FLOAT pmass1, INT min, INT max);
static INT  DSLIM_BinFindMax(pepEntry *entries, FLOAT pmass2, INT min, INT max);
static inline STATUS DSLIM_WaitFor_IO(INT &);
static inline STATUS DSLIM_Deinit_IO();

/* FUNCTION: DSLIM_WaitFor_IO
 *
 * DESCRIPTION:
 *
 * INPUT:
 * @none
 *
 * OUTPUT:
 * @status: status of execution
 *
 */
static inline STATUS DSLIM_WaitFor_IO(INT &batchsize)
{
    STATUS status;

    batchsize = 0;

    /* Wait for a I/O request */
    status = qPtrs->lockr_();

    /* Check for either a buffer or stopSignal */
    while (qPtrs->isEmptyReadyQ())
    {
        /* Check if endSignal has been received */
        if (SchedHandle->checkSignal())
        {
            status = qPtrs->unlockr_();
            status = ENDSIGNAL;
            break;
        }

        /* If both conditions fail,
         * the I/O threads still working */
        status = qPtrs->unlockr_();

        sleep(0.1);

        status = qPtrs->lockr_();
    }

    if (status == SLM_SUCCESS)
    {
        /* Get the I/O ptr from the wait queue */
        workPtr = qPtrs->getWorkPtr();

        if (workPtr == NULL)
        {
            status = ERR_INVLD_PTR;
        }

        status = qPtrs->unlockr_();

        batchsize = workPtr->numSpecs;
    }

    return status;
}

/* FUNCTION: DSLIM_SearchManager
 *
 * DESCRIPTION: Manages and performs the Peptide Search
 *
 * INPUT:
 * @slm_index : Pointer to the SLM_Index
 *
 * OUTPUT:
 * @status: Status of execution
 */
static STATUS DSLIM_InitializeMS2Data()
{
    STATUS status = SLM_SUCCESS;

    INT nfiles = queryfiles.size();
    ptrs = new MSQuery*[nfiles];

    /* Initialize the queue with already created nfiles */
    qfPtrs = new lwqueue<MSQuery*>(nfiles, false);

#ifdef _OPENMP
#pragma omp parallel for schedule (dynamic, 1)
#endif/* _OPENMP */
    for (auto fid = 0; fid < nfiles; fid++)
    {
        ptrs[fid] = new MSQuery;
        ptrs[fid]->InitQueryFile(&queryfiles[fid], fid);
    }

    /* Push zeroth as is */
    qfPtrs->push(ptrs[0]);
    dssize += ptrs[0]->QAcount;

    /* Update batch numbers */
    for (auto fid = 1; fid < nfiles; fid++)
    {
        ptrs[fid]->curr_chunk = ptrs[fid - 1]->curr_chunk + ptrs[fid - 1]->nqchunks;
        qfPtrs->push(ptrs[fid]);
        dssize += ptrs[fid]->QAcount;
    }

    /* Compute the total number of batches in the dataset */
    nBatches = ptrs[nfiles-1]->curr_chunk + ptrs[nfiles-1]->nqchunks;


#ifndef DIAGNOSE
                if (params.myid == 0)
                {
                    //std::cout << "\nQuery File: " << queryfiles[qfid_lcl] << endl;
                    //std::cout << "Elapsed Time: " << elapsed_seconds.count() << "s" << endl << endl;
                }
#endif /* DIAGNOSE */

#ifdef BENCHMARK
                fileio += omp_get_wtime() - duration;
#endif /* BENCHMARK */

    return status;
}


/* FUNCTION: DSLIM_SearchManager
 *
 * DESCRIPTION: Manages and performs the Peptide Search
 *
 * INPUT:
 * @slm_index : Pointer to the SLM_Index
 *
 * OUTPUT:
 * @status: Status of execution
 */
STATUS DSLIM_SearchManager(Index *index)
{
    STATUS status = SLM_SUCCESS;
    INT batchsize = 0;

    auto start = chrono::system_clock::now();
    auto end   = chrono::system_clock::now();
    chrono::duration<double> elapsed_seconds = end - start;
    chrono::duration<double> qtime = end - start;
    INT maxlen = params.max_len;
    INT minlen = params.min_len;

#ifdef DISTMEM
    THREAD *wthread = new THREAD;
#endif /* DISTMEM */

    /* The mutex for queryfile vector */
    if (status == SLM_SUCCESS)
    {
        status = sem_init(&qfilelock, 0, 1);

        status = DSLIM_InitializeMS2Data();
    }

    /* Initialize the lw double buffer queues with
     * capacity, min and max thresholds */
    if (status == SLM_SUCCESS)
    {
        qPtrs = new lwbuff<Queries>(20, 5, 15); // cap, th1, th2
    }

    /* Initialize the ePtrs */
    if (status == SLM_SUCCESS)
    {
        ePtrs = new expeRT[params.threads];
    }

    /* Create queries buffers and push them to the lwbuff */
    if (status == SLM_SUCCESS)
    {
        /* Create new Queries */
        for (INT wq = 0; wq < qPtrs->len(); wq++)
        {
            Queries *nPtr = new Queries;

            /* Initialize the query buffer */
            nPtr->init();

            /* Add them to the buffer */
            qPtrs->Add(nPtr);
        }
    }

    if (status == SLM_SUCCESS)
    {
        /* Let's do a queue of 10 MSQuery elements -
         * should be more than enough */

        ioQ = new lwqueue<MSQuery*>(10);

        /* Check for correct allocation */
        if (ioQ == NULL)
        {
            status = ERR_BAD_MEM_ALLOC;
        }

        /* Initialize the ioQlock */
        status = sem_init(&ioQlock, 0, 1);
    }

    if (status == SLM_SUCCESS && params.nodes == 1)
    {
        status = DFile_InitFiles();
    }
#ifdef DISTMEM
    else if (status == SLM_SUCCESS && params.nodes > 1)
    {
        iBuff = new ebuffer[NIBUFFS];

        status = sem_init(&writer, 0, 0);

        if (wthread != NULL && status == SLM_SUCCESS)
        {
            /* Pass the reference to thread block as argument */
            status = pthread_create(wthread, NULL, &DSLIM_FOut_Thread_Entry, (VOID *)NULL);
        }
    }
#endif /* DISTMEM */

    /* Initialize the Comm module */
#ifdef DISTMEM

    /* Only required if nodes > 1 */
    if (params.nodes > 1)
    {
        /* Allocate a new DSLIM Comm handle */
        if (status == SLM_SUCCESS)
        {
            CommHandle = new DSLIM_Comm(nBatches);

            if (CommHandle == NULL)
            {
                status = ERR_BAD_MEM_ALLOC;
            }
        }

        if (status == SLM_SUCCESS)
        {
            CandidatePSMS = new hCell[dssize];

            if (CandidatePSMS == NULL)
            {
                status = ERR_BAD_MEM_ALLOC;
            }
        }
    }

#endif /* DISTMEM */

    /* Create a new Scheduler handle */
    if (status == SLM_SUCCESS)
    {
        SchedHandle = new Scheduler;

        /* Check for correct allocation */
        if (SchedHandle == NULL)
        {
            status = ERR_BAD_MEM_ALLOC;
        }
    }

    /**************************************************************************/
    /* The main query loop starts here */
    while (status == SLM_SUCCESS)
    {
        /* Start computing penalty */
        auto spen = chrono::system_clock::now();

        status = DSLIM_WaitFor_IO(batchsize);

        /* Check if endsignal */
        if (status == ENDSIGNAL)
        {
            break;
        }

        /* Compute the penalty */
        chrono::duration<double> penalty = chrono::system_clock::now() - spen;

#ifndef DIAGNOSE
        if (params.myid == 0)
        {
            std::cout << "PENALTY: " << penalty.count() << endl;
        }
#endif /* DIAGNOSE */

        /* Check the status of buffer queues */
        qPtrs->lockr_();
        INT dec = qPtrs->readyQStatus();
        qPtrs->unlockr_();

        /* Run the Scheduler to manage thread between compute and I/O */
        SchedHandle->runManager(penalty.count(), dec);

#ifndef DIAGNOSE
        if (params.myid == 0)
        {
            std::cout << "Querying: \n" << endl;
        }
#endif /* DIAGNOSE */

        start = chrono::system_clock::now();

        if (status == SLM_SUCCESS)
        {
            /* Query the chunk */
            status = DSLIM_QuerySpectrum(workPtr, index, (maxlen - minlen + 1));
        }

#ifdef DISTMEM
        /* Transfer my partial results to others */
        if (status == SLM_SUCCESS && params.nodes > 1)
        {
            status = sem_post(&writer);
        }
#endif /* DISTMEM */

        status = qPtrs->lockw_();

        /* Request next I/O chunk */
        qPtrs->Replenish(workPtr);

        status = qPtrs->unlockw_();

        end = chrono::system_clock::now();

        /* Compute Duration */
        qtime += end - start;

#ifndef DIAGNOSE
        if (params.myid == 0)
        {
            /* Compute Duration */
            std::cout << "\nQuery Time: " << qtime.count() << "s" << endl;
            std::cout << "Queried with status:\t\t" << status << endl << endl;
        }
#endif /* DIAGNOSE */

        end = chrono::system_clock::now();
    }

    /* Deinitialize the IO module */
    status = DSLIM_Deinit_IO();

    /* Delete the scheduler object */
    if (SchedHandle != NULL)
    {
        /* Deallocate the scheduler module */
        delete SchedHandle;

        SchedHandle = NULL;
    }

#ifdef DISTMEM
    /* Deinitialize the Communication module */
    if (params.nodes > 1)
    {
        ciBuff ++;
        ebuffer *liBuff = iBuff + (ciBuff % NIBUFFS);

        /* Wait for FOut thread to take out iBuff */
        for (; liBuff->isDone == false; usleep(1000));

        status = sem_post(&writer);

        VOID *ptr = NULL;

        pthread_join(*wthread, &ptr);

        delete wthread;

        sem_destroy(&writer);

        if (iBuff != NULL)
        {
            delete[] iBuff;
            iBuff = NULL;
        }

#ifdef DIAGNOSE
        std::cout << "ExitSignal: " << params.myid << endl;
#endif /* DIAGNOSE */

        /* Wait for everyone to synchronize */
        status = MPI_Barrier(MPI_COMM_WORLD);

        /* Carry forward the data to the distributed scoring module */
        status = DSLIM_CarryForward(index, CommHandle, ePtrs, CandidatePSMS, spectrumID);

        /* Delete the instance of CommHandle */
        if (CommHandle != NULL)
        {
            delete CommHandle;
            CommHandle = NULL;
        }

    }
#endif /* DISTMEM */

    if (status == SLM_SUCCESS && params.nodes == 1)
    {
        status = DFile_DeinitFiles();

        delete[] ePtrs;
        ePtrs = NULL;
    }

    /* Return the status of execution */
    return status;
}

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
STATUS DSLIM_QuerySpectrum(Queries *ss, Index *index, UINT idxchunk)
{
    STATUS status = SLM_SUCCESS;
    UINT maxz = params.maxz;
    UINT dF = params.dF;
    INT threads = (INT)params.threads - (INT)SchedHandle->getNumActivThds();
    UINT scale = params.scale;
    DOUBLE maxmass = params.max_mass;
    ebuffer *liBuff = NULL;
    partRes *txArray = NULL;

    if (params.nodes > 1)
    {
        ciBuff ++;
        liBuff = iBuff + (ciBuff % NIBUFFS);

        /* Wait for FOut thread to take out iBuff */
        for (; liBuff->isDone == false; usleep(10000));

        txArray = liBuff->packs;
        liBuff->isDone = false;
        liBuff->batchNum = ss->batchNum;
    }
    else
    {
        LBE_UNUSED_PARAM(liBuff);
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

    /* Sanity checks */
    if (Score == NULL || (txArray == NULL && params.nodes > 1))
    {
        status = ERR_INVLD_MEMORY;
    }

    if (status == SLM_SUCCESS)
    {
        /* Should at least be 1 and min 75% */
        INT minthreads = MAX(1, (params.threads * 3)/4);

        threads = MAX(threads, minthreads);

#ifndef DIAGNOSE
        /* Print how many threads are we using here */
        if (params.myid == 0)
        {
            /* Print the number of query threads */
            std::cout << "\n#QThds: " << threads << endl;
        }
#endif /* DIAGNOSE */

        /* Process all the queries in the chunk.
         * Setting chunk size to 4 to avoid false sharing
         */
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(dynamic, 4)
#endif /* _OPENMP */
        for (INT queries = 0; queries < ss->numSpecs; queries++)
        {
#ifdef BENCHMARK
            DOUBLE stime = omp_get_wtime();
#endif
            /* Pointer to each query spectrum */
            UINT *QAPtr = ss->moz + ss->idx[queries];
            FLOAT pmass = ss->precurse[queries];
            UINT *iPtr = ss->intensity + ss->idx[queries];
            UINT qspeclen = ss->idx[queries + 1] - ss->idx[queries];
            UINT thno = omp_get_thread_num();

            BYC *bycPtr = Score[thno].byc;
            iBYC *ibycPtr = Score[thno].ibyc;
            Results *resPtr = &Score[thno].res;
            expeRT  *expPtr = ePtrs + thno;
            ebuffer *inBuff = inBuff + thno;

#ifndef DIAGNOSE
            if (thno == 0 && params.myid == 0)
            {
                std::cout << "\rDONE: " << (queries * 100) /ss->numSpecs << "%";
            }
#endif /* DIAGNOSE */

            for (UINT ixx = 0; ixx < idxchunk; ixx++)
            {
                UINT speclen = (index[ixx].pepIndex.peplen - 1) * maxz * iSERIES;
                UINT halfspeclen = speclen / 2;

                for (UINT chno = 0; chno < index[ixx].nChunks; chno++)
                {
                    /* Query each chunk in parallel */
                    UINT *bAPtr = index[ixx].ionIndex[chno].bA;
                    UINT *iAPtr = index[ixx].ionIndex[chno].iA;

                    INT minlimit = 0;
                    INT maxlimit = 0;

                    BOOL val = DSLIM_BinarySearch(index + ixx, ss->precurse[queries], minlimit, maxlimit);

                    /* Spectrum violates limits */
                    if (val == false || (maxlimit < minlimit))
                    {
                        continue;
                    }

                    /* Query all fragments in each spectrum */
                    for (UINT k = 0; k < qspeclen; k++)
                    {
                        /* Do this to save mem boundedness */
                        auto qion = QAPtr[k];

                        /* Check for any zeros
                         * Zero = Trivial query */
                        if (qion > dF && qion < ((maxmass * scale) - 1 - dF))
                        {
                            for (auto bin = qion - dF; bin < qion + 1 + dF; bin++)
                            {
                                /* Locate iAPtr start and end */
                                UINT start = bAPtr[bin];
                                UINT end = bAPtr[bin + 1];

                                /* If no ions in the bin */
                                if (end - start < 1)
                                {
                                    continue;
                                }

                                auto ptr = std::lower_bound(iAPtr + start, iAPtr + end, minlimit * speclen);
                                INT stt = start + std::distance(iAPtr + start, ptr);

                                ptr = std::upper_bound(iAPtr + stt, iAPtr + end, (((maxlimit + 1) * speclen) - 1));
                                INT ends = stt + std::distance(iAPtr + stt, ptr) - 1;

                                /* Loop through located iAions */
                                for (auto ion = stt; ion <= ends; ion++)
                                {
                                    UINT raw = iAPtr[ion];
                                    UINT intn = iPtr[k];

                                    /* Calculate parent peptide ID */
                                    INT ppid = (raw / speclen);

                                    /* b-ion matched */
                                    if ((raw % speclen) < halfspeclen)
                                    {
                                        bycPtr[ppid].bc += 1;
                                        ibycPtr[ppid].ibc += intn;
                                    }
                                    /* y-ion matched */
                                    else
                                    {
                                        bycPtr[ppid].yc += 1;
                                        ibycPtr[ppid].iyc += intn;
                                    }
                                }
                            }
                        }
                    }

                    /* Compute the chunksize to look further into */
                    INT csize = maxlimit - minlimit + 1;

                    /* Look for candidate PSMs */
                    for (INT it = minlimit; it <= maxlimit; it++)
                    {
                        USHORT bcc = bycPtr[it].bc;
                        USHORT ycc = bycPtr[it].yc;
                        USHORT shpk = bcc + ycc;

                        /* Filter by the min shared peaks */
                        if (shpk >= params.min_shp)
                        {
                            ULONGLONG pp = UTILS_Factorial(bcc) *
                                    UTILS_Factorial(ycc);

                            /* Create a heap cell */
                            hCell cell;

                            /* Fill in the information */
                            cell.hyperscore = 0.001 + pp * ibycPtr[it].ibc * ibycPtr[it].iyc;

                            cell.hyperscore = log10(cell.hyperscore) - 6;

                            /* hyperscore < 0 means either b- or y- ions were not matched */
                            if (cell.hyperscore > 0)
                            {
                                cell.idxoffset = ixx;
                                cell.psid = it;
                                cell.sharedions = shpk;
                                cell.totalions = speclen;
                                cell.pmass = pmass;

                                /* Insert the cell in the heap dst */
                                resPtr->topK.insert(cell);

                                /* Increase the N */
                                resPtr->cpsms += 1;

                                /* Update the histogram */
                                resPtr->survival[(INT) (cell.hyperscore * 10 + 0.5)] += 1;
                            }
                        }
                    }

                    /* Clear the scorecard */
                    std::memset(bycPtr + minlimit, 0x0, sizeof(BYC) * csize);
                    std::memset(ibycPtr + minlimit, 0x0, sizeof(iBYC) * csize);
                }
            }

#ifdef DISTMEM
            /* Distributed memory mode - Model partial Gumbel
             * and transmit parameters to rx machine */
            if (params.nodes > 1)
            {
                /* Set the params.min_cpsm in dist mem mode to 1 */
                if (resPtr->cpsms >= 1)
                {
                    /* Extract the top PSM */
                    hCell psm = resPtr->topK.getMax();

                    /* Put it in the list */
                    CandidatePSMS[spectrumID + queries] = psm;

                    resPtr->maxhypscore = (psm.hyperscore * 10 + 0.5);

                    status = expPtr->StoreIResults(resPtr, queries, liBuff);

                    /* Fill in the Tx array cells */
                    txArray[queries].min  = resPtr->minhypscore;
                    txArray[queries].max2 = resPtr->nexthypscore;
                    txArray[queries].max  = resPtr->maxhypscore;
                    txArray[queries].N    = resPtr->cpsms;
                    txArray[queries].qID  = spectrumID + queries;
                }
                else
                {
                    /* No need to memset as there are apt checks in dslim_score.cpp
                    memset(liBuff->ibuff + (queries * 128 * sizeof(USHORT)), 0x0, 128 * sizeof(USHORT));*/

                    /* Extract the top result
                     * and put it in the list */
                    CandidatePSMS[spectrumID + queries] = 0;

                    /* Get the handle to the txArr
                     * Fill it up and move on */
                    txArray[queries] = 0;
                    txArray[queries].qID  = spectrumID + queries;
                }
            }

            /* Shared memory mode - Do complete
             * modeling and print results */
            else
#endif /* DISTMEM */
            {
                /* Check for minimum number of PSMs */
                if (resPtr->cpsms >= params.min_cpsm)
                {
                    /* Extract the top PSM */
                    hCell psm = resPtr->topK.getMax();

                    resPtr->maxhypscore = (psm.hyperscore * 10 + 0.5);

                    /* Compute expect score if there
                     * are any candidate PSMs */
#ifdef TAILFIT
                    status = expPtr->ModelTailFit(resPtr);

                    /* Linear Regression Parameters */
                    DOUBLE w = resPtr->mu;
                    DOUBLE b = resPtr->beta;

                    w /= 1e6;
                    b /= 1e6;

                    /* Estimate the log (s(x)); x = log(hyperscore) */
                    DOUBLE lgs_x = (w * resPtr->maxhypscore) + b;

                    /* Compute the s(x) */
                    DOUBLE e_x = pow(10, lgs_x);

                    /* e(x) = n * s(x) */
                    e_x *= resPtr->cpsms;

#else
                    status = expPtr->ModelSurvivalFunction(resPtr);

                    /* Extract e(x) = n * s(x) = mu * 1e6 */
                    DOUBLE e_x = resPtr->mu;

                    e_x /= 1e6;

#endif /* TAILFIT */

                    /* Do not print any scores just yet */
                    if (e_x < params.expect_max)
                    {
                        /* Printing the scores in OpenMP mode */
                        status = DFile_PrintScore(index, spectrumID + queries, pmass, &psm, e_x, resPtr->cpsms);
                    }
                }
            }

            /* Reset the results */
            resPtr->reset();

#ifdef BENCHMARK
            tcons[thno] += omp_get_wtime() - stime;
#endif
        }

        if (params.nodes > 1)
        {
            liBuff->currptr = ss->numSpecs * 128 * sizeof(USHORT);
        }

        /* Update the number of queried spectra */
        spectrumID += ss->numSpecs;
    }

#ifdef BENCHMARK
    compute += omp_get_wtime() - duration;

    for (unsigned int thd = 0; thd < params.threads; thd++)
    {
        std::cout << "\nThread #: " << thd << "\t" << tcons[thd];
    }
#endif

#ifndef DIAGNOSE
    if (params.myid == 0)
    {
        std::cout << "\nQueried Spectra:\t\t" << workPtr->numSpecs << endl;
    }
#endif /* DIAGNOSE */

    return status;
}

/*
 * FUNCTION: DSLIM_BinarySearch
 *
 * DESCRIPTION: The Binary Search Algorithm
 *
 * INPUT:
 *
 * OUTPUT
 * none
 */
static BOOL DSLIM_BinarySearch(Index *index, FLOAT precmass, INT &minlimit, INT &maxlimit)
{
    /* Get the FLOAT precursor mass */
    FLOAT pmass1 = precmass - params.dM;
    FLOAT pmass2 = precmass + params.dM;
    pepEntry *entries = index->pepEntries;

    BOOL rv = false;

    UINT min = 0;
    UINT max = index->lcltotCnt - 1;

    if (params.dM < 0.0)
    {
        minlimit = min;
        maxlimit = max;

        return rv;
    }

    /* Check for base case */
    if (pmass1 < entries[min].Mass)
    {
        minlimit = min;
    }
    else if (pmass1 > entries[max].Mass)
    {
        minlimit = max;
        maxlimit = max;
        return rv;
    }
    else
    {
        /* Find the minlimit here */
        minlimit = DSLIM_BinFindMin(entries, pmass1, min, max);
    }

    min = 0;
    max = index->lcltotCnt - 1;


    /* Check for base case */
    if (pmass2 > entries[max].Mass)
    {
        maxlimit = max;
    }
    else if (pmass2 < entries[min].Mass)
    {
        minlimit = min;
        maxlimit = min;
        return rv;
    }
    else
    {
        /* Find the maxlimit here */
        maxlimit = DSLIM_BinFindMax(entries, pmass2, min, max);
    }

    if (entries[maxlimit].Mass <= pmass2 && entries[minlimit].Mass >= pmass1)
    {
        rv = true;
    }

    return rv;
}


static INT DSLIM_BinFindMin(pepEntry *entries, FLOAT pmass1, INT min, INT max)
{
    INT half = (min + max)/2;

    if (max - min < 20)
    {
        INT current = min;

        while (entries[current].Mass < pmass1)
        {
            current++;
        }

        return current;
    }

    if (pmass1 > entries[half].Mass)
    {
        min = half;
        return DSLIM_BinFindMin(entries, pmass1, min, max);
    }
    else if (pmass1 < entries[half].Mass)
    {
        max = half;
        return DSLIM_BinFindMin(entries, pmass1, min, max);
    }

    if (pmass1 == entries[half].Mass)
    {
        while (pmass1 == entries[half].Mass)
        {
            half--;
        }

        half++;
    }

    return half;


}

static INT DSLIM_BinFindMax(pepEntry *entries, FLOAT pmass2, INT min, INT max)
{
    INT half = (min + max)/2;

    if (max - min < 20)
    {
        INT current = max;

        while (entries[current].Mass > pmass2)
        {
            current--;
        }

        return current;
    }

    if (pmass2 > entries[half].Mass)
    {
        min = half;
        return DSLIM_BinFindMax(entries, pmass2, min, max);
    }
    else if (pmass2 < entries[half].Mass)
    {
        max = half;
        return DSLIM_BinFindMax(entries, pmass2, min, max);
    }

    if (pmass2 == entries[half].Mass)
    {
        half++;

        while (pmass2 == entries[half].Mass)
        {
            half++;
        }

        half--;
    }

    return half;

}
/*
 * FUNCTION: DSLIM_IO_Threads_Entry
 *
 * DESCRIPTION: Entry function for all
 *              I/O threads
 *
 * INPUT:
 * @argv: Pointer to void arguments
 *
 * OUTPUT:
 * @NULL: Nothing
 */
VOID *DSLIM_IO_Threads_Entry(VOID *argv)
{
    STATUS status = SLM_SUCCESS;
    auto start = chrono::system_clock::now();
    auto end   = chrono::system_clock::now();
    chrono::duration<double> elapsed_seconds = end - start;
    chrono::duration<double> qtime = end - start;
    Queries *ioPtr = NULL;

    BOOL eSignal = false;

    /* local object is fine since it will be copied
     * to the queue object at the time of preemption */
    MSQuery *Query = NULL;

    INT rem_spec = 0;

    while (SchedHandle == NULL);

    /* Initialize and process Query Spectra */
    for (;status == SLM_SUCCESS;)
    {

#ifdef BENCHMARK
        duration = omp_get_wtime();
#endif /* BENCHMARK */

        /* Check if the Query object is not initialized */
        if (Query == NULL || Query->isDeInit())
        {
            /* Try getting the Query object from queue if present */
            status = sem_wait(&ioQlock);

            if (!ioQ->isEmpty())
            {
                Query = ioQ->front();
                status = ioQ->pop();
            }

            status = sem_post(&ioQlock);
        }

        /* If the queue is empty */
        if (Query == NULL || Query->isDeInit())
        {
            /* Otherwise, initialize the object from a file */
            start = chrono::system_clock::now();

            /* lock the query file */
            sem_wait(&qfilelock);

            /* Check if anymore queryfiles */
            if (!qfPtrs->isEmpty())
            {
                Query = qfPtrs->front();
                qfPtrs->pop();
                rem_spec = Query->getQAcount(); // Init to 1 for first loop to run
            }
            else
            {
                /* Raise the exit signal */
                eSignal = true;
            }

            /* Unlock the query file */
            sem_post(&qfilelock);
        }

        /* If no more files, then break the inf loop */
        if (eSignal == true)
        {
            break;
        }

        /*********************************************
         * At this point, we have the data ready     *
         *********************************************/

        /* All set - Run the DSLIM Query Algorithm */
        if (status == SLM_SUCCESS)
        {
            start = chrono::system_clock::now();
#ifdef BENCHMARK
            duration = omp_get_wtime();
#endif /* BENCHMARK */

            /* Wait for a I/O request */
            status = qPtrs->lockw_();

            /* Empty wait queue or Scheduler preemption signal raised  */
            if (SchedHandle->checkPreempt() || qPtrs->isEmptyWaitQ())
            {
                status = qPtrs->unlockw_();

                if (status == SLM_SUCCESS)
                {
                    status = sem_wait(&ioQlock);
                }

                if (status == SLM_SUCCESS)
                {
                    status = ioQ->push(Query);
                }

                if (status == SLM_SUCCESS)
                {
                    status = sem_post(&ioQlock);
                }

                /* Break from loop */
                break;
            }

            /* Otherwise, get the I/O ptr from the wait queue */
            ioPtr = qPtrs->getIOPtr();

            status = qPtrs->unlockw_();

            /* Reset the ioPtr */
            ioPtr->reset();

            /* Extract a chunk and return the chunksize */
            status = Query->ExtractQueryChunk(QCHUNK, ioPtr, rem_spec);

            ioPtr->batchNum = Query->curr_chunk;
            Query->curr_chunk++;

            /* Lock the ready queue */
            qPtrs->lockr_();

#ifdef DISTMEM
            if (params.nodes > 1)
            {
                /* Add an entry of the added buffer to the CommHandle */
                status = CommHandle->AddBatch(ioPtr->batchNum, ioPtr->numSpecs, Query->getQfileIndex());
            }
#endif /* DISTMEM */

            /*************************************
             * Add available data to ready queue *
             *************************************/
            qPtrs->IODone(ioPtr);

            /* Unlock the ready queue */
            qPtrs->unlockr_();

#ifdef BENCHMARK
            fileio += omp_get_wtime() - duration;
#endif /* BENCHMARK */
            end = chrono::system_clock::now();

            /* Compute Duration */
            elapsed_seconds = end - start;

#ifndef DIAGNOSE
            if (params.myid == 0)
            {
                std::cout << "\nExtracted Spectra :\t\t" << ioPtr->numSpecs << endl;
                std::cout << "Elapsed Time: " << elapsed_seconds.count() << "s" << endl << endl;
            }
#endif /* DIAGNOSE */

            /* If no more remaining spectra, then deinit */
            if (rem_spec < 1)
            {
                status = Query->DeinitQueryFile();

                if (Query != NULL)
                {
                    delete Query;
                    Query = NULL;
                }
            }
        }
        /*else
        {
            std::cout << "ALERT: IOTHD @" << params.myid << endl;
        }*/
    }

    /* Check if we ran out of files */
    if (eSignal == true)
    {
        if (Query != NULL)
        {
            delete Query;
            Query = NULL;
        }

        /* Free the main IO thread */
        SchedHandle->ioComplete();
    }

    /* Request pre-emption */
    SchedHandle->takeControl(argv);

    return NULL;
}

#ifdef DISTMEM
VOID *DSLIM_FOut_Thread_Entry(VOID *argv)
{
    //STATUS status = SLM_SUCCESS;
    INT batchSize = 0;
    INT clbuff = -1;

    for (;;)
    {
        /*status = */sem_wait(&writer);

        clbuff += 1;
        ebuffer *lbuff = iBuff + (clbuff % NIBUFFS);

        if (lbuff->isDone == true)
        {
            //cout << "FOut_Thread_Exiting @: " << params.myid <<endl;
            break;
        }

        ofstream *fh = new ofstream;
        STRING fn = params.datapath + "/" +
                    std::to_string(lbuff->batchNum) +
                    "_" + std::to_string(params.myid) + ".dat";

        batchSize = lbuff->currptr / (128 * sizeof(USHORT));

        fh->open(fn, ios::out | ios::binary);

        fh->write((CHAR *)lbuff->packs, batchSize * sizeof(partRes));
        fh->write(lbuff->ibuff, lbuff->currptr * sizeof (CHAR));

        fh->close();

        lbuff->isDone = true;
    }

    return argv;
}
#endif /* DISTMEM */

static inline STATUS DSLIM_Deinit_IO()
{
    STATUS status = SLM_SUCCESS;

    Queries *ptr = NULL;

    while (!qPtrs->isEmptyReadyQ())
    {
        ptr = qPtrs->getWorkPtr();

        if (ptr != NULL)
        {
            delete ptr;
            ptr = NULL;
        }
    }

    while (!qPtrs->isEmptyWaitQ())
    {
        ptr = qPtrs->getIOPtr();

        if (ptr != NULL)
        {
            delete ptr;
            ptr = NULL;
        }
    }

    /* Delete the qPtrs buffer handle */
    delete qPtrs;

    qPtrs = NULL;

    /* Deallocate the I/O queues */
    delete ioQ;
    ioQ = NULL;

    /* Destroy the ioQ lock semaphore */
    status = sem_destroy(&ioQlock);

    return status;
}