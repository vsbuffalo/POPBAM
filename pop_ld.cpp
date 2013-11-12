/** \file pop_ld.cpp
 *  \brief Functions for calculating linkage disequilibrium statistics
 *  \author Daniel Garrigan
 *  \version 0.4
*/
#include "pop_ld.h"
#include "tables.h"

int main_ld(int argc, char *argv[])
{
	int k = 0;
	int chr = 0;                  //! chromosome identifier
	int beg = 0;                  //! beginning coordinate for analysis
	int end = 0;                  //! end coordinate for analysis
	int ref = 0;                  //! ref
	long num_windows = 0;         //! number of windows
	long cw = 0;                  //! counter for windows
	std::string msg;              //! string for error message
	std::string region;           //! the scaffold/chromosome region string
	bam_plbuf_t *buf = nullptr;   //! pileup buffer
	ldData *t = nullptr;          //! pointer to the function data structure

	// allocate memory for nucdiv data structre
	t = new ldData;

	// parse the command line options
	region = t->parseCommandLine(argc, argv);

	// check input BAM file for errors
	t->checkBAM();

	// initialize the sample data structure
	t->bam_smpl_init();

	// add samples
	t->bam_smpl_add();

	// initialize error model
	t->em = errmod_init(0.17);

	// parse genomic region
	k = bam_parse_region(t->h, region, &chr, &beg, &end);
	if (k < 0)
	{
		msg = "Bad genome coordinates: " + region;
		fatalError(msg);
	}

	// fetch reference sequence
	t->ref_base = faidx_fetch_seq(t->fai_file, t->h->target_name[chr], 0, 0x7fffffff, &(t->len));

	// calculate the number of windows
	if (t->flag & BAM_WINDOW)
		num_windows = ((end - beg) - 1) / t->win_size;
	else
	{
		t->win_size = end - beg;
		num_windows = 1;
	}

	// iterate through all windows along specified genomic region
	for (cw=0; cw < num_windows; cw++)
	{

		// construct genome coordinate string
		std::string scaffold_name(t->h->target_name[chr]);
		std::ostringstream winc(scaffold_name);
		winc.seekp(0, std::ios::end);
		winc << ":" << beg + (cw * t->win_size) + 1 << "-" << ((cw + 1) * t->win_size) + (beg - 1);
		std::string winCoord = winc.str();

		// initialize number of sites to zero
		t->num_sites = 0;

		// parse the BAM file and check if region is retrieved from the reference
		if (t->flag & BAM_WINDOW)
		{
			k = bam_parse_region(t->h, winCoord, &ref, &(t->beg), &(t->end));
			if (k < 0)
			{
				msg = "Bad window coordinates " + winCoord;
				fatalError(msg);
			}
		}
		else
		{
			ref = chr;
			t->beg = beg;
			t->end = end;
			if (ref < 0)
			{
				msg = "Bad scaffold name: " + region;
				fatalError(msg);
			}
		}

		// initialize nucdiv variables
		t->init_ld();

		// create population assignments
		t->assign_pops();

		// initialize pileup
		buf = bam_plbuf_init(make_ld, t);

		// fetch region from bam file
		if ((bam_fetch(t->bam_in->x.bam, t->idx, ref, t->beg, t->end, buf, fetch_func)) < 0)
		{
			msg = "Failed to retrieve region " + region + " due to corrupted BAM index file";
			fatalError(msg);
		}

		// finalize pileup
		bam_plbuf_push(0, buf);

		// calculate linkage disequilibrium statistics
		ld_func fp[3] = {&ldData::calc_zns, &ldData::calc_omegamax, &ldData::calc_wall};
		(t->*fp[t->output])();

		// print results to stdout
		t->print_ld(chr);

		// take out the garbage
		t->destroy_ld();
		bam_plbuf_destroy(buf);
	}
	// end of window interation

	errmod_destroy(t->em);
	samclose(t->bam_in);
	bam_index_destroy(t->idx);
	t->bam_smpl_destroy();
	free(t->ref_base);
	delete t;

	return 0;
}

int make_ld(unsigned int tid, unsigned int pos, int n, const bam_pileup1_t *pl, void *data)
{
	int i = 0;
	int fq = 0;
	unsigned long long sample_cov = 0;
	unsigned long long *cb = nullptr;
	ldData *t = nullptr;

	// get control data structure
	t = (ldData*)data;

	// only consider sites located in designated region
	if ((t->beg <= (int)pos) && (t->end > (int)pos))
	{
		// allocate memory pileup data
		try
		{
			cb = new unsigned long long [t->sm->n]();
		}
		catch (std::bad_alloc& ba)
		{
			std::cerr << "bad_alloc caught: " << ba.what() << std::endl;
		}

		// call bases
		t->call_base(n, pl, cb);

		// resolve heterozygous sites
		if (!(t->flag & BAM_HETEROZYGOTE))
			clean_heterozygotes(t->sm->n, cb, (int)t->ref_base[pos], t->min_snpQ);

		// determine if site is segregating
		fq = segbase(t->sm->n, cb, t->ref_base[pos], t->min_snpQ);

		// determine how many samples pass the quality filters
		sample_cov = qfilter(t->sm->n, cb, t->min_rmsQ, t->min_depth, t->max_depth);

		// determine population coverage
		for (i=0; i < t->sm->npops; ++i)
		{
			unsigned long long pc = 0;
			pc = sample_cov & t->pop_mask[i];
			unsigned int ncov = bitcount64(pc);
			if (ncov == t->pop_nsmpl[i])
				t->pop_cov[t->num_sites] |= 0x1U << i;
		}

		// record site type if the site is variable
		if (t->pop_cov[t->num_sites] > 0)
		{
			t->num_sites++;
			if (fq > 0)
				t->types[t->segsites++] = cal_site_type(t->sm->n, cb);
		}

		// take out the garbage
		delete [] cb;
	}
	return 0;
}

void ldData::calc_zns(void)
{
	int i = 0;
	int j = 0;
	int k = 0;
	unsigned short n = 0;
	unsigned short x0 = 0;
	unsigned short x1 = 0;
	unsigned long long x11 = 0;
	unsigned long long type0 = 0;
	unsigned long long type1 = 0;

	if (segsites < 1)
		return;

	// iterate through populations
	for (i=0; i < sm->npops; i++)
	{
		// Zero the SNP counter
		num_snps[i] = 0;
		n = pop_nsmpl[i];

		// iterate through segregating sites
		for (j=0; j < segsites-1; j++)
		{
			// get first population-specific site and count of the "derived" allele
			type0 = types[j] & pop_mask[i];
			x0 = bitcount64(type0);

			// if site 1 is variable within the population of interest
			if ((x0 >= min_freq) && (x0 <= (n - min_freq)))
			{
				// iterate SNP counter
				++num_snps[i];

				// iterate over all remaining sites
				for (k=j+1; k < segsites; k++)
				{
					// get second population-specific site and count of the "derived" allele
					type1 = types[k] & pop_mask[i];
					x1 = bitcount64(type1);

					// if site 2 is variable within the population of interest -> calculate r2
					if ((x1 >= min_freq) && (x1 <= (n - min_freq)))
					{
						x11 = bitcount64(type0 & type1);
						zns[i] += SQ(x0 * x1 - n * x11) / (double)((n - x0) * x0 * (n - x1) * x1);
					}
				}
			}
		}
		++num_snps[i];

		// get average pairwise r2
		zns[i] *= 1.0 / BINOM(num_snps[i]);
		// end pairwise comparisons
	}
}

void ldData::calc_omegamax(void)
{
	int i, j, k, m, n;
	int count1 = 0;
	int count2 = 0;
	int left = 0;
	int right = 0;
	unsigned short x0 = 0;
	unsigned short x1 = 0;
	unsigned long long type0 = 0;
	unsigned long long type1 = 0;
	unsigned long long x11 = 0;
	double **r2 = nullptr;
	double sumleft = 0.0;
	double sumright = 0.0;
	double sumbetween = 0.0;
	double omega = 0.0;

	if (segsites < 1)
		return;

	for (j=0; j < sm->npops; j++)
	{
		try
		{
			r2 = new double* [segsites];
			for (i=0; i < segsites; i++)
				r2[i] = new double [segsites]();
		}
		catch (std::bad_alloc& ba)
		{
			std::cerr << "bad_alloc caught: " << ba.what() << std::endl;
		}

		// Zero the pairwise comparison counter
		num_snps[j] = 0;
		count1 = 0;
		count2 = 0;
		n = pop_nsmpl[j];

		for (i=0; i < segsites-1; i++)
		{
			type0 = types[i] & pop_mask[j];
			x0 = bitcount64(type0);

			// if site 1 is variable within the population of interest
			if ((x0 >= min_freq) && (x0 <= (n - min_freq)))
			{
				++num_snps[j];
				count2 = count1;
				for (k=i+1; k < segsites; k++)
				{
					type1 = types[k] & pop_mask[j];
					x1 = bitcount64(type1);

					// if site 2 is variable within the population of interest
					if ((x1 >= min_freq) && (x1 <= (n - min_freq)))
					{
						++count2;

						// calculate r2
						x11 = bitcount64(type0 & type1);
						r2[count1][count2] = SQ(x0 * x1 - n * x11) / (double)((n - x0) * x0 * (n - x1) * x1);
						r2[count2][count1] = r2[count1][count2];
					}
				}
				++count1;
			}
		}
		++num_snps[j];
		// end pairwise comparisons

		// omegamax calculation

		// initialize sums and omegamax
		sumleft = 0;
		sumright = 0;
		sumbetween = 0;
		omegamax[j] = 0;

		// consider all partitions of r2 matrix
		for (i=1; i < num_snps[j]-1; i++)
		{

			// sum over SNPs to the left
			for (k=0; k < i; k++)
				for (m=k+1; m <= i; m++)
					sumleft += r2[k][m];

			// sum over SNPs on either side
			for (k=i+1; k < num_snps[j]; k++)
				for (m=0; m <= i; m++)
					sumbetween += r2[k][m];

			// sum over SNPs to the right
			for (k=i+1; k < num_snps[j]-1; k++)
				for (m=k+1; m < num_snps[j]; m++)
					sumright += r2[k][m];

			// get numbers of SNPs in the partition
			left = i + 1;
			right = num_snps[j] - left;

			// calculate omega for current partition
			omega = (sumleft + sumright) / (BINOM(left) + BINOM(right));
			omega *= left * right / sumbetween;

			// update omega max
			omegamax[j] = omega > omegamax[j] ? omega : omegamax[j];
		}

		// take out the garbage
		for (i=0; i < segsites; i++)
			delete [] r2[i];
		delete [] r2;
	}
}

void ldData::calc_wall(void)
{
	int i, j, k;
	unsigned long long last_type = 0;
	int *num_congruent = nullptr;
	int *num_part = nullptr;
	unsigned long long type = 0;
	unsigned long long complem = 0;
	std::vector<std::vector<unsigned long long> > uniq_part_types(sm->npops);

	if (segsites < 1)
		return;

	try
	{
		num_congruent = new int [sm->npops]();
		num_part = new int [sm->npops]();
	}
	catch (std::bad_alloc& ba)
	{
		std::cerr << "bad_alloc caught: " << ba.what() << std::endl;
	}

	for (i=0; i < segsites; i++)
	{
		for (j=0; j < sm->npops; j++)
		{
			// initialize population specific type and its complement
			type = 0;
			complem = 0;

			// define bit mask variables
			for (k=0; k < sm->n; k++)
			{
				if (CHECK_BIT(types[i],k) & CHECK_BIT(pop_mask[j],k))
					type |= 0x1ULL << k;
				else if (~CHECK_BIT(types[i],k) & CHECK_BIT(pop_mask[j],k))
					complem |= 0x1ULL << k;
				else
					continue;
			}

			// if the site is variable within the population of interest
			if ((type > 0) && (type < pop_mask[j]))
			{

				// is it the first segregating site?
				if (num_snps[j] == 0)
				{
					uniq_part_types[j].push_back(type);
					last_type = type;
					num_snps[j]++;
				}
				else
				{
					if ((type == last_type) || (complem == last_type))
					{
						num_congruent[j]++;
						int x = count(uniq_part_types[j].begin(), uniq_part_types[j].end(), type);
						int y = count(uniq_part_types[j].begin(), uniq_part_types[j].end(), complem);
						if ((x == 0) && (y == 0))
						{
							uniq_part_types[j].push_back(type);
							num_part[j]++;
						}
					}
					num_snps[j]++;
					last_type = type;
				}
			}
		}
	}

	// calculate Wall's B statistic for each population
	for (i=0; i < sm->npops; i++)
	{
		wallb[i] = (double)(num_congruent[i]) / (num_snps[i] - 1);
		wallq[i] = (double)(num_congruent[i] + num_part[i]) / num_snps[i];
	}

	// take out the garbage
	delete [] num_congruent;
	delete [] num_part;
}

std::string ldData::parseCommandLine(int argc, char *argv[])
{
#ifdef _MSC_VER
	struct _stat finfo;
#else
	struct stat finfo;
#endif
	std::vector<std::string> glob_opts;
	std::string msg;

	GetOpt::GetOpt_pp args(argc, argv);
	args >> GetOpt::Option('f', reffile);
	args >> GetOpt::Option('h', headfile);
	args >> GetOpt::Option('m', min_depth);
	args >> GetOpt::Option('x', max_depth);
	args >> GetOpt::Option('q', min_rmsQ);
	args >> GetOpt::Option('s', min_snpQ);
	args >> GetOpt::Option('a', min_mapQ);
	args >> GetOpt::Option('b', min_baseQ);
	args >> GetOpt::Option('o', output);
	args >> GetOpt::Option('z', het_prior);
	args >> GetOpt::Option('n', min_snps);
	args >> GetOpt::Option('w', win_size);
	args >> GetOpt::Option('k', min_sites);
	if (args >> GetOpt::OptionPresent('w'))
	{
		win_size *= KB;
		flag |= BAM_WINDOW;
	}
	if (args >> GetOpt::OptionPresent('h'))
		flag |= BAM_HEADERIN;
	if (args >> GetOpt::OptionPresent('i'))
		flag |= BAM_ILLUMINA;
	if (args >> GetOpt::OptionPresent('e'))
		min_freq = 2;
	args >> GetOpt::GlobalOption(glob_opts);

	// run some checks on the command line

	// check if output option is valid
	if ((output < 0) || (output > 2))
		printUsage("Not a valid output option");

	// if no input BAM file is specified -- print usage and exit
	if (glob_opts.size() < 2)
		printUsage("Need to specify input BAM file name");
	else
		bamfile = glob_opts[0];

	// check if specified BAM file exists on disk
	if ((stat(bamfile.c_str(), &finfo)) != 0)
	{
		msg = "Specified input file: " + bamfile + " does not exist";
		switch(errno)
		{
		case ENOENT:
			std::cerr << "File not found" << std::endl;
			break;
		case EINVAL:
			std::cerr << "Invalid parameter to stat" << std::endl;
			break;
		default:
			std::cerr << "Unexpected error in stat" << std::endl;
			break;
		}
		fatalError(msg);
	}

	// check if fastA reference file is specified
	if (reffile.empty())
		printUsage("Need to specify fastA reference file");

	// check is fastA reference file exists on disk
	if ((stat(reffile.c_str(), &finfo)) != 0)
	{
		switch(errno)
		{
		case ENOENT:
			std::cerr << "File not found" << std::endl;
			break;
		case EINVAL:
			std::cerr << "Invalid parameter to stat" << std::endl;
			break;
		default:
			std::cerr << "Unexpected error in stat" << std::endl;
			break;
		}
		msg = "Specified reference file: " + reffile + " does not exist";
		fatalError(msg);
	}

	//check if BAM header input file exists on disk
	if (flag & BAM_HEADERIN)
	{
		if ((stat(headfile.c_str(), &finfo)) != 0)
		{
			switch(errno)
			{
			case ENOENT:
				std::cerr << "File not found" << std::endl;
				break;
			case EINVAL:
				std::cerr << "Invalid parameter to stat" << std::endl;
				break;
			default:
				std::cerr << "Unexpected error in stat" << std::endl;
				break;
			}
			msg = "Specified header file: " + headfile + " does not exist";
			fatalError(msg);
		}
	}

	// return the index of first non-optioned argument
	return glob_opts[1];
}

ldData::ldData(void)
{
	derived_type = LD;
	output = 0;
	min_snps = 10;
	min_freq = 1;
	win_size = 0;
	min_sites = 0.5;
}

void ldData::init_ld(void)
{
	int length = end - beg;
	int npops = sm->npops;

	segsites = 0;

	try
	{
		types = new unsigned long long [length]();
		pop_mask = new unsigned long long [npops]();
		pop_nsmpl = new unsigned char [npops]();
		pop_cov = new unsigned int [length]();
		num_snps = new int [npops]();
		switch (output)
		{
		case 0:
			zns = new double [npops]();
			break;
		case 1:
			omegamax = new double [npops]();
			break;
		case 2:
			wallb = new double [npops]();
			wallq = new double [npops]();
			break;
		default:
			zns = new double [npops]();
			break;
		}
	}
	catch (std::bad_alloc& ba)
	{
		std::cerr << "bad_alloc caught: " << ba.what() << std::endl;
	}
}

void ldData::print_ld(int chr)
{
	int i = 0;

	//print coordinate information and number of aligned sites
	std::cout << h->target_name[chr] << "\t" << beg + 1 << "\t" << end + 1 << "\t" << num_sites;

	//print results for each population
	for (i=0; i < sm->npops; i++)
	{
		std::cout << "\tS[" << sm->popul[i] << "]:\t" << num_snps[i];

		//If window passes the minimum number of SNPs filter
		if (num_snps[i] >= min_snps)
		{
			switch (output)
			{
			case 0:
				std::cout << "\tZns[" << sm->popul[i] << "]:";
				std::cout << "\t" << std::fixed << std::setprecision(5) << zns[i];
				break;
			case 1:
				std::cout << "\tomax[" << sm->popul[i] << "]:";
				std::cout << "\t" << std::fixed << std::setprecision(5) << omegamax[i];
				break;
			case 2:
				std::cout << "\tB[" << sm->popul[i] << "]:";
				std::cout << "\t" << std::fixed << std::setprecision(5) << wallb[i];
				std::cout << "\tQ[" << sm->popul[i] << "]:";
				std::cout << "\t" << std::fixed << std::setprecision(5) << wallq[i];
				wallb[i] = 0.0;
				wallq[i] = 0.0;
				break;
			default:
				std::cout << "\tZns[" << sm->popul[i] << "]:";
				std::cout << "\t" << std::fixed << std::setprecision(5) << zns[i];
				break;
			}
		}
		else
		{
			switch(output)
			{
			case 0:
				std::cout << "\tZns[" << sm->popul[i] << "]:\t" << std::setw(7) << "NA";
				break;
			case 1:
				std::cout << "\tomax[" << sm->popul[i] << "]:\t" << std::setw(7) << "NA";
				break;
			case 2:
				std::cout << "\tB[" << sm->popul[i] << "]:\t" << std::setw(7) << "NA";
				std::cout << "\tQ[" << sm->popul[i] << "]:\t" << std::setw(7) << "NA";
				wallb[i] = 0.0;
				wallq[i] = 0.0;
				break;
			default:
				std::cout << "\tZns[" << sm->popul[i] << "]:\t" << std::setw(7) << "NA";
				break;
			}
		}
	}
	std::cout << std::endl;
}

void ldData::destroy_ld(void)
{
	delete [] pop_mask;
	delete [] types;
	delete [] pop_nsmpl;
	delete [] pop_cov;
	delete [] num_snps;
	switch(output)
	{
	case 0:
		delete[] zns;
		break;
	case 1:
		delete [] omegamax;
		break;
	case 2:
		delete [] wallb;
		delete [] wallq;
		break;
	default:
		delete [] zns;
		break;
	}
}

void ldData::printUsage(std::string msg)
{
	std::cerr << msg << std::endl << std::endl;
	std::cerr << "Usage:   popbam ld [options] <in.bam> [region]" << std::endl;
	std::cerr << std::endl;
	std::cerr << "Options: -i          base qualities are Illumina 1.3+               [ default: Sanger ]" << std::endl;
	std::cerr << "         -h  FILE    Input header file                              [ default: none ]" << std::endl;
	std::cerr << "         -e          exclude singletons from LD calculations        [ default: include singletons ]" << std::endl;
	std::cerr << "         -o  INT     analysis option                                [ default: 0 ]" << std::endl;
	std::cerr << "                     0 : Kelly's ZnS statistic" << std::endl;
	std::cerr << "                     1 : Omega max" << std::endl;
	std::cerr << "                     2 : Wall's B and Q congruency statistics" << std::endl;
	std::cerr << "         -w  INT     use sliding window of size (kb)" << std::endl;
	std::cerr << "         -k  FLT     minimum proportion of aligned sites in window  [ default: 0.5 ]" << std::endl;
	std::cerr << "         -f  FILE    reference fastA file" << std::endl;
	std::cerr << "         -n  INT     mimimum number of snps to consider window      [ default: 10 ]" << std::endl;
	std::cerr << "         -m  INT     minimum read coverage                          [ default: 3 ]" << std::endl;
	std::cerr << "         -x  INT     maximum read coverage                          [ default: 255 ]" << std::endl;
	std::cerr << "         -q  INT     minimum rms mapping quality                    [ default: 25 ]" << std::endl;
	std::cerr << "         -s  INT     minimum snp quality                            [ default: 25 ]" << std::endl;
	std::cerr << "         -a  INT     minimum map quality                            [ default: 13 ]" << std::endl;
	std::cerr << "         -b  INT     minimum base quality                           [ default: 13 ]" << std::endl;
	std::cerr << std::endl;
	exit(EXIT_FAILURE);
}
