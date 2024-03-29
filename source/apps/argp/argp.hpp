/*
 * Copyright (C) 2022 Muhammad Haseeb, Fahad Saeed
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
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <filesystem>
#include <optional>
#include <iostream>
#include <iomanip>
#include <ctime>
#include <thread>
#include <algorithm>
#include <sys/stat.h>

#include "common.hpp"
#include "slm_dsts.h"
#include "slmerr.h"
#include "argparse/argparse.hpp"

#ifdef USE_MPI
#include <mpi.h>
#endif // USE_MPI

namespace hcp
{
namespace apps
{
namespace argp
{

// function to get current time and date
auto getcurrtimeanddate()
{
    // use current time for the workspace if not available
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::stringstream timeanddate;
    timeanddate << std::put_time(&tm, "%m.%d.%Y.%H.%M.%S");
    return timeanddate.str();
}

auto getcurrpath()
{
    // COMPILER VERSION GCC 9.1.0+ required
#if __GNUC__ > 9 || (__GNUC__ == 9 && (__GNUC_MINOR__ >= 1))
    // COMPILER VERSION GCC 9.1.0+ required for std::filesystem calls
    static string_t currpath = std::filesystem::current_path();
#else
    char tmp[1024];
    getcwd(tmp, 1024);
    static string_t currpath = string_t(tmp);
#endif // __GNUC__ > 9

    return currpath;
}

template <typename T>
auto sanitize_res(T &res)
{
    // sanitize resolution
    if (res <= 0) res = static_cast<T>(0.01);
    else if (res > static_cast<T>(5.0)) res = static_cast<T>(5.0);
}

template <typename T>
auto sanitize_nmods(T &nmods)
{
    // sanitize resolution
    if (nmods <= static_cast<T>(0)) nmods = static_cast<T>(1);
    else if (nmods > static_cast<T>(7)) nmods = static_cast<T>(7);
}

template <typename T>
auto sanitize_dM(T &dM)
{
    // sanitize resolution
    if (dM < static_cast<T>(0)) dM = static_cast<T>(MBYTES(1));
}


//
// structure to store parsed params
//
struct params_t : public argparse::Args
{

    //
    // make sure all default values are of the same type as the variable
    // e.g. float&->20f, double&->20.0, int&->20, short&->(short)20
    //

    // either provide working directory or the current directory is assumed
    std::optional<string_t> &workdir    = kwarg("c,wdir", "path to working directory");

    // database will be uploaded at the working directory in SGCI
    string_t &dbpath                     = kwarg("db,database", "path to processed database files (*.peps)").set_default(workdir.value_or(getcurrpath()));

    // dataset will be uploaded at the working directory in SGCI
    string_t &dataset                    = kwarg("dat, dataset", "path to MS/MS dataset (*.ms2)").set_default(workdir.value_or(getcurrpath()));

    // path to a new workspace or make one at currdir)
    string_t &workspace                  = kwarg("w,workspace", "path to the output workspace").set_default(workdir.value_or(getcurrpath()) + "/hicops_workspace_" + getcurrtimeanddate());

    // maximum threads to use per HiCOPS instance
    int &threads                         = kwarg("t,threads", "maximum number of threads per HiCOPS instance").set_default(std::max(1, static_cast<int>(std::thread::hardware_concurrency())));

    // max threads for preprocessing subtask R per HiCOPS instance
    int &prepthreads                     = kwarg("p,prep_threads", "maximum allowed threads for subtask-R per HiCOPS instance").set_default(std::max(1, static_cast<int>(std::thread::hardware_concurrency()/3)));

    // max gpu threads to run at once
    int &gputhreads                      = kwarg("g,gputhreads", "maximum allowed threads simultaneously offloading to GPU").set_default(8);

    // number of mods per peptide
    int &nmods                           = kwarg("n,nmods", "allowed maximum PTMs per peptide").set_default(3);

    // min pep len
    int &minlength                       = kwarg("lmin,min_length", "minimum peptide sequence length").set_default(6);

    // max pep len
    int &maxlength                       = kwarg("lmax,max_length", "maximum peptide sequence length").set_default(40);

    // max fragment z
    int &maxz                            = kwarg("z,maxz", "maximum theoretical fragment ion charge").set_default(3);

    // min mass
    double &minprecmass                  = kwarg("minmass,min_prec_mass", "minimum MS/MS spectrum precursor mass").set_default(500.0);

    // max mass
    double &maxprecmass                  = kwarg("maxmass,max_prec_mass", "maximum MS/MS spectrum precursor mass").set_default(5000.0);

    // min shared peaks for PSM candidacy
    int &min_shp                         = kwarg("shp,min_shp", "minimum shared peaks for PSM candidacy").set_default(4);

    int &topmatches                      = kwarg("top,topmatches", "number of top PSMs to print in the output (inactive option)").set_default(1);

    // min PSMs for expect modeling
    int &hits                            = kwarg("hits,min_hits", "minimum candidate PSMs for e-value modeling").set_default(4);

    // base intensity x1000
    int &base_int                        = kwarg("base,base_int", "base noramlized peak intensity for MS/MS data x1000").set_default(1000);

    // MS/MS peak cut off ratio
    double &cutoff                       = kwarg("cutoff_ratio", "cutoff peak ratio wrt base intensity (e.g. 1% = 0.01)").set_default(0.01);

    // m/z axis resolution
    double &resolution                   = kwarg("res", "x-axis (m/z axis) resolution in Da in range: [0.01, 5.0]").set_default(0.01);

    // dM
    double &deltaM                       = kwarg("dM", "peptide precursor mass tolerance (+-Da)").set_default(10.0);

    // dF
    double &deltaF                       = kwarg("dF", "fragment-ion mass tolerance (+-Da)").set_default(0.02);

    // max e-value to report
    double &maxexpect                    = kwarg("e_max,expect_max", "maximum expect value (e-value) to report").set_default(20.0);

    // LBE distribution policy

    // DistPolicy_t requires magic_enum submodule.
    DistPolicy_t &lbe_policy             = kwarg("policy", "LBE Distribution policy (cyclic, chunk, zigzag)").set_default(DistPolicy_t::cyclic);

    // scratch pad memory in MB
    int &bufferMBs                       = kwarg("buff,spad_mem", "buffer (scratch pad) RAM memory in MB (recommended: 2048MB+)").set_default(2048);

    // this should be an optional parameter
    std::optional<std::vector<std::string>> &mods
                                         = kwarg("m,mods", "list of variable post-translational modifications (PTMs)").multi_argument();

    // do not keep the full database index on GPU
    bool &nogpuindex                     = flag("ngi,nogpuindex", "GiCOPS: do not keep full database index on GPU");

    // re-index MS/MS data and create and index
    bool &reindex                        = flag("reindex", "rebuild/update the MS/MS dataset index");

    // do not build or use MS/MS cache
    bool &nocache                        = flag("nocache", "do not cache preprocessed MS/MS dataset to .pbin");

    // use GumbelFit / Survival function modeling instead of TailFit for e_value computation
    bool &gumbelfit                      = flag("e,gfit", "use GumbelFit/Survival instead of TailFit to compute e-values");

    // match ion when doing fragment ion matching
    bool &matchcharge                    = flag("matchz", "matching ion charges during fragment-ion search");

    // do not show database search progress marks
    bool &progress                       = flag("noprogress", "do not display progress marks");

    // toggle verbose mode
    bool &verbose                        = flag("v,V,verbose", "enable verbose mode");
};

auto &get_instance(int argc, char* argv[])
{
    static auto instance = argparse::parse<params_t>(argc, argv);
    return instance;
}

auto &get_instance() { return get_instance(0, nullptr); }

void getParams(gParams &params)
{
    // get the static instance of parser. Use 0 and nullptr as not needed anymore.
    auto parser = get_instance();

    params.dbpath = parser.dbpath;
    params.datapath = parser.dataset;
    params.workspace = parser.workspace;

    // set the fullgIndex if not disabled
    params.gpuindex = !parser.nogpuindex;

    // auto sanitize and set data extension
    params.setindexAndCache(parser.reindex, parser.nocache);

#if !defined(ARGP_ONLY)

    // COMPILER VERSION GCC 9.1.0+ required
#if __GNUC__ > 9 || (__GNUC__ == 9 && (__GNUC_MINOR__ >= 1))
    // COMPILER VERSION GCC 9.1.0+ required for std::filesystem calls
    std::filesystem::create_directory(parser.workspace);
#else
    mkdir(parser.workspace.c_str(), 0777);
#endif // __GNUC__ > 9

#endif // !ARGP_ONLY


#ifdef USE_OMP
    params.threads = parser.threads;
#else
    params.threads = 1;
#endif /* USE_OMP */

#ifdef USE_OMP
        params.maxprepthds = parser.prepthreads;
#else
        params.maxprepthds = 1;
#endif /* USE_OMP */

#ifdef USE_GPU
        params.gputhreads = parser.gputhreads;
#else
        params.gputhreads = 0;
        params.useGPU = false;
#endif // USE_GPU

        // Get the min peptide length
        params.min_len = parser.minlength;

        // Get the max peptide length
        params.max_len = parser.maxlength;

        /* Get the max fragment charge */
        params.maxz = parser.maxz;

        // Get the m/z axis resolution and sanitize it if needed
        params.res = parser.resolution;
        sanitize_res(params.res);

        // compute the scaling factor
        params.scale = static_cast<int>(static_cast<double>(1.0)/params.res);

        // Get the fragment mass tolerance x scale
        params.dF = parser.deltaF * params.scale;

        // Get the precursor mass tolerance
        params.dM = parser.deltaM;
        sanitize_dM(params.dM);

        // Get the min mass
        params.min_mass = parser.minprecmass;

        // Get the max mass
        params.max_mass = parser.maxprecmass;

        // Get the top matches to report
        params.topmatches = parser.topmatches;

        // Get the max expect score to report
        params.expect_max = parser.maxexpect;

        /* Get the shp threshold */
        params.min_shp = parser.min_shp;

        /* Get the minhits threshold */
        params.min_cpsm = parser.hits;

        // Base Intensity x 1000
        params.base_int = parser.base_int * YAXISMULTIPLIER;

        // Cutoff intensity ratio (add 0.5 for nearest rounding)
        params.min_int = static_cast<double_t>(params.base_int) * parser.cutoff + 0.5;

        // Get the scorecard + scratch memory in MBs
        params.spadmem = MBYTES(parser.bufferMBs);

        // Get the LBE distribution policy
        params.policy = parser.lbe_policy;

        // Get number of mods per peptide
        params.vModInfo.vmods_per_pep = parser.nmods;
        sanitize_nmods(params.vModInfo.vmods_per_pep);

        // get the total number of mods and the mods vector
        if (parser.mods.has_value())
        {
            // get the list of mods
            auto modslist = parser.mods.value();

            // set the number of mods
            params.vModInfo.num_vars = modslist.size();
            params.modconditions = std::to_string(params.vModInfo.vmods_per_pep);

            // process the strings: AA:MASS.0:NUM
            for (auto md = 0; md < params.vModInfo.num_vars; md++)
            {
                // for each mod string
                auto &mod = modslist[md];

                // remove any whitespaces
                mod.erase(std::remove_if(mod.begin(), mod.end(), ::isspace), mod.end());

                // replace colons with space
                std::replace(mod.begin(), mod.end(), ':', ' ');

                // tokenize the string
                std::stringstream modtokens(mod);
                string_t element;

                // extract the AAs
                modtokens >> element;

                // append to the modconditions string
                params.modconditions += " " + element;

                // copy to vmods
                std::strncpy((char *) params.vModInfo.vmods[md].residues, (const char *) element.c_str(),
                        std::min(4, static_cast<int>(element.length())));

                // extract the Mass
                modtokens >> element;

                // copy to vmods
                params.vModInfo.vmods[md].modMass = (uint_t) (std::atof((const char *) element.c_str()) * params.scale);

                // extract the NUM
                modtokens >> element;

                // append to the modconditions string
                params.modconditions += " " + element;

                // copy to vmods
                params.vModInfo.vmods[md].aa_per_peptide = std::atoi((const char *) element.c_str());

            }
        }
        else
        {
            params.vModInfo.num_vars = 0;
            params.modconditions = "0";
        }

#if defined(USE_MPI) && !(defined(ARGP_NOMPI) || defined(ARGP_ONLY))
    MPI_Comm_rank(MPI_COMM_WORLD, (int_t *)&params.myid);
    MPI_Comm_size(MPI_COMM_WORLD, (int_t *)&params.nodes);
#else
    params.myid = 0;
    params.nodes = 1;
#endif /* USE_MPI */

}

void parseAndgetParams(int argc, char *argv[], gParams &params)
{
    auto parser = get_instance(argc, argv);

    // print help if no arguments provided
    if (argc < 2)
    {
        parser.help();
        exit(0);
    }

    if (parser.verbose)
        parser.print();

    return getParams(params);
}

void printParser()
{
    // get the static instance of parser. Use 0 and nullptr as not needed anymore.
    auto parser = get_instance();

    // get database path
    printVar(parser.dbpath);

    // get dataset path
    printVar(parser.dataset);

    // get workspace path
    printVar(parser.workspace);

    // get max number of threads
    printVar(parser.threads);

    // get max preprocessing threads
    printVar(parser.prepthreads);

    // Get the min peptide length
    printVar(parser.minlength);

    // Get the max peptide length
    printVar(parser.maxlength);

    /* Get the max fragment charge */
    printVar(parser.maxz);

    // Get the m/z axis resolution
    printVar(parser.resolution);

    // Get the fragment mass tolerance
    printVar(parser.deltaF);

    // Get the precursor mass tolerance
    printVar(parser.deltaM);

    // Get the min mass
    printVar(parser.minprecmass);

    // Get the max mass
    printVar(parser.maxprecmass);

    // Get the top matches to report
    printVar(parser.topmatches);

    // Get the max expect score to report
    printVar(parser.maxexpect);

    /* Get the shp threshold */
    printVar(parser.min_shp);

    /* Get the minhits threshold */
    printVar(parser.hits);

    // Base Intensity x 1000
    printVar(parser.base_int);

    // Cutoff intensity ratio
    printVar(parser.cutoff);

    // Get the scorecard + scratch memory in MBs
    printVar(parser.bufferMBs);

    // Get the LBE distribution policy
    printVar(parser.lbe_policy);

    // Get number of mods per peptide
    printVar(parser.nmods);

    // get the total number of mods and the mods vector
    if (parser.mods.has_value())
    {
        auto modsvect = parser.mods.value();
        for (auto &mod : modsvect)
        {
            std::cout <<"mod = " << mod << std::endl;
        }
    }
}

} // namespace hcp
} // namespace apps
} // namespace argp

