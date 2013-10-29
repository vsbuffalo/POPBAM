/** \file pop_sfs.cpp
 *  \brief Functions for calculating frequency spectrum statistics
 *  \author Daniel Garrigan
 *  \version 0.4
*/
#include "pop_sfs.h"
#include "tables.h"

int main_sfs(int argc, char *argv[])
{
	bool found = false;       //! is the outgroup sequence found?
	int i;
	int chr;                  //! chromosome identifier
	int beg;                  //! beginning coordinate for analysis
	int end;                  //! end coordinate for analysis
	int ref;                  //! ref
	long num_windows;         //! number of windows
	std::string msg;          //! string for error message
	bam_plbuf_t *buf;         //! pileup buffer
	sfsData t;

	// parse the command line options
	std::string region = t.parseCommandLine(argc, argv);

	// check input BAM file for errors
	t.checkBAM();

	// initialize the sample data structure
	t.bam_smpl_init();

	// add samples
	t.bam_smpl_add();

	// initialize error model
	t.em = errmod_init(1.-0.83);

	// if outgroup option is used check to make sure it exists
	if (t.flag & BAM_OUTGROUP)
	{
		for (i=0; i < t.sm->n; i++)
		{
			if (strcmp(t.sm->smpl[i], t.outgroup.c_str()) == 0)
			{
				t.outidx = i;
				found = true;
			}
		}
		unsigned long long u = 0x1ULL << t.outidx;
		for (i=0; i < t.sm->npops; ++i)
			if (t.pop_mask[i] & u)
				t.outpop = i;
		if (!found)
		{
			msg = "Specified outgroup " + t.outgroup + " not found";
			fatal_error(msg, __FILE__, __LINE__, 0);
		}
	}

	// calculate the constants for computation of Tajima's D
	t.calc_a1();
	t.calc_a2();
	t.calc_e1();
	t.calc_e2();
	t.calc_dw();
	t.calc_hw();

	// parse genomic region
	int k = bam_parse_region(t.h, region, &chr, &beg, &end);
	if (k < 0)
	{
		msg = "Bad genome coordinates: " + region;
		fatal_error(msg, __FILE__, __LINE__, 0);
	}

	// fetch reference sequence
	t.ref_base = faidx_fetch_seq(t.fai_file, t.h->target_name[chr], 0, 0x7fffffff, &(t.len));

	// calculate the number of windows
	if (t.flag & BAM_WINDOW)
		num_windows = ((end-beg)-1)/t.win_size;
	else
	{
		t.win_size = end - beg;
		num_windows = 1;
	}

	// iterate through all windows along specified genomic region
	for (long cw=0; cw < num_windows; cw++)
	{
		// construct genome coordinate string
		std::string scaffold_name(t.h->target_name[chr]);
		std::ostringstream winc(scaffold_name);
		winc.seekp(0, std::ios::end);
		winc << ":" << beg+(cw*t.win_size)+1 << "-" << ((cw+1)*t.win_size)+(beg-1);
		std::string winCoord = winc.str();

		// initialize number of sites to zero
		t.num_sites = 0;

		// parse the BAM file and check if region is retrieved from the reference
		if (t.flag & BAM_WINDOW)
		{
			k = bam_parse_region(t.h, winCoord, &ref, &(t.beg), &(t.end));
			if (k < 0)
			{
				msg = "Bad window coordinates " + winCoord;
				fatal_error(msg, __FILE__, __LINE__, 0);
			}
		}
		else
		{
			ref = chr;
			t.beg = beg;
			t.end = end;
			if (ref < 0)
			{
				msg = "Bad scaffold name: " + region;
				fatal_error(msg, __FILE__, __LINE__, 0);
			}
		}

		// initialize nucdiv variables
		t.init_sfs();

		// create population assignments
		t.assign_pops();

		// initialize pileup
		buf = bam_plbuf_init(make_sfs, &t);

		// fetch region from bam file
		if ((bam_fetch(t.bam_in->x.bam, t.idx, ref, t.beg, t.end, buf, fetch_func)) < 0)
		{
			msg = "Failed to retrieve region " + region + " due to corrupted BAM index file";
			fatal_error(msg, __FILE__, __LINE__, 0);
		}

		// finalize pileup
		bam_plbuf_push(0, buf);

		// calculate site frequency spectrum statistics
		t.calc_sfs();

		// print results to stdout
		t.print_sfs(chr);

		// take out the garbage
		t.destroy_sfs();
		bam_plbuf_destroy(buf);
	}
	// end of window interation

	errmod_destroy(t.em);
	samclose(t.bam_in);
	bam_index_destroy(t.idx);
	t.bam_smpl_destroy();
	for (i=0; i <= t.sm->n; ++i)
	{
		delete [] t.dw[i];
		delete [] t.hw[i];
	}
	delete [] t.dw;
	delete [] t.hw;
	delete [] t.a1;
	delete [] t.a2;
	delete [] t.e1;
	delete [] t.e2;
	free(t.ref_base);

	return 0;
}

int make_sfs(unsigned int tid, unsigned int pos, int n, const bam_pileup1_t *pl, void *data)
{
	int i;
	int fq;
	unsigned long long sample_cov;
	unsigned long long *cb = NULL;
	sfsData *t = NULL;

	// get control data structure
	t = (sfsData*)data;

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

		unsigned int *ncov;
		ncov = new unsigned int [t->sm->npops]();

		// determine population coverage
		for (i=0; i < t->sm->npops; ++i)
		{
			unsigned long long pc = 0;
			pc = sample_cov & t->pop_mask[i];
			ncov[i] = bitcount64(pc);
			unsigned int req = (unsigned int)((t->min_pop * t->pop_nsmpl[i]) + 0.4999);
			if (ncov[i] >= req)
				t->pop_cov[t->num_sites] |= 0x1U << i;
		}

		// record site type if the site is variable
		if (t->pop_cov[t->num_sites] > 0)
		{
			t->num_sites++;
			if (fq > 0)
			{
				for (int j=0; j < t->sm->npops; ++j)
					t->ncov[j][t->segsites] = ncov[j];
				t->types[t->segsites++] = cal_site_type(t->sm->n, cb);
			}
		}

		// take out the garbage
		delete [] cb;
		delete [] ncov;
	}
	return 0;
}

void sfsData::calc_sfs(void)
{
	int i, j;
	int n, s;
	int avgn;
	unsigned short freq;
	unsigned long long pop_type;

	// count number of aligned sites in each population
	for (i=0; i < num_sites; ++i)
		for (j=0; j < sm->npops; ++j)
			if (CHECK_BIT(pop_cov[i],j))
				++ns[j];

	for (i=0; i < sm->npops; i++)
	{
		if (ns[i] >= (unsigned long int)((end - beg) * min_sites))
		{
			// get site frequency spectra and number of segregating sites
			num_snps[i] = 0;
			avgn = 0;
			for (j=0; j < segsites; j++)
			{
				pop_type = types[j] & pop_mask[i];

				// check if outgroup is aligned and different from reference
				// else the reference base is assumed to be ancestral
				if ((flag & BAM_OUTGROUP) && (ncov[outpop][j] > 0) && CHECK_BIT(types[j], outidx))
					freq = ncov[i][j] - bitcount64(pop_type);
				else
					freq = bitcount64(pop_type);

				if ((freq > 0) && (freq < ncov[i][j]))
				{
					td[i] += dw[ncov[i][j]][freq];
					fwh[i] += hw[ncov[i][j]][freq];
					avgn += ncov[i][j];
					++num_snps[i];
				}
			}

			// finalize calculation of sfs statistics
			n = (int)(((double)(avgn)/num_snps[i])+0.4999);
			s = num_snps[i];
			td[i] /= sqrt(e1[n] * s + e2[n] * s * (s - 1));
			fwh[i] /= sqrt(((n - 2) * (s / a1[n]) / (6.0 * (n - 1))) + ((s * (s - 1) / (SQ(a1[n]) + a2[n])) * 
				      (18.0 * SQ(n) * (3.0 * n + 2.0) * a2[n+1] - (88.0 * n * n * n + 9.0 * SQ(n) - 13.0 * n + 6.0)) / (9.0 * n * (SQ(n-1)))));
		}
		else
		{
			td[i] = std::numeric_limits<double>::quiet_NaN();
			fwh[i] = std::numeric_limits<double>::quiet_NaN();
		}
	}
}

void sfsData::print_sfs(int chr)
{
	int i;

	std::cout << h->target_name[chr] << "\t" << beg+1 << "\t" << end+1;
	for (i=0; i < sm->npops; i++)
	{
		std::cout << "\tns[" << sm->popul[i] << "]:\t" << ns[i];
		if (isnan(td[i]))
			std::cout << "\tD[" << sm->popul[i] << "]:\t" << std::setw(7) << "NA";
		else
		{
			std::cout << "\tD[" << sm->popul[i] << "]:";
			std::cout << "\t" << std::fixed << std::setprecision(5) << td[i];
		}
		if (isnan(fwh[i]))
			std::cout << "\tH[" << sm->popul[i] << "]:\t" << std::setw(7) << "NA";
		else
		{
			std::cout << "\tH[" << sm->popul[i] << "]:";
			std::cout << "\t" << std::fixed << std::setprecision(5) << fwh[i];
		}
	}
	std::cout << std::endl;
}

std::string sfsData::parseCommandLine(int argc, char *argv[])
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
	args >> GetOpt::Option('p', outgroup);
	args >> GetOpt::Option('s', min_snpQ);
	args >> GetOpt::Option('a', min_mapQ);
	args >> GetOpt::Option('b', min_baseQ);
	args >> GetOpt::Option('k', min_sites);
	args >> GetOpt::Option('n', min_pop);
	args >> GetOpt::Option('w', win_size);
	if (args >> GetOpt::OptionPresent('w'))
	{
		win_size *= KB;
		flag |= BAM_WINDOW;
	}
	if (args >> GetOpt::OptionPresent('h'))
		flag |= BAM_HEADERIN;
	if (args >> GetOpt::OptionPresent('p'))
		flag |= BAM_OUTGROUP;
	if (args >> GetOpt::OptionPresent('i'))
		flag |= BAM_ILLUMINA;
	args >> GetOpt::GlobalOption(glob_opts);

	// run some checks on the command line

	// if no input BAM file is specified -- print usage and exit
	if (glob_opts.size() < 2)
	{
		msg = "Need to specify input BAM file name";
		fatal_error(msg, __FILE__, __LINE__, &sfsUsage);
	}
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
		fatal_error(msg, __FILE__, __LINE__, 0);
	}

	// check if fastA reference file is specified
	if (reffile.empty())
	{
		msg = "Need to specify fasta reference file name";
		fatal_error(msg, __FILE__, __LINE__, &sfsUsage);
	}

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
		fatal_error(msg, __FILE__, __LINE__, 0);
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
			fatal_error(msg, __FILE__, __LINE__, 0);
		}
	}

	// return the index of first non-optioned argument
	return glob_opts[1];
}

sfsData::sfsData(void)
{
	derived_type = SFS;
	min_sites = 0.5;
	outidx = 0;
	win_size = 0;
	min_pop = 1.0;
}

void sfsData::init_sfs(void)
{
	int i;
	int length = end-beg;
	int npops = sm->npops;

	segsites = 0;

	try
	{
		types = new unsigned long long [length]();
		ns = new unsigned long int [npops]();
		ncov = new unsigned int* [npops];
		pop_mask = new unsigned long long [npops]();
		pop_nsmpl = new unsigned char [npops]();
		pop_cov = new unsigned int [length]();
		num_snps = new int [npops]();
		td = new double [npops]();
		fwh = new double [npops]();
		for (i=0; i < npops; ++i)
			ncov[i] = new unsigned int [length]();
	}
	catch (std::bad_alloc& ba)
	{
		std::cerr << "bad_alloc caught: " << ba.what() << std::endl;
	}
}

void sfsData::destroy_sfs(void)
{
	int i;
	int npops = sm->npops;

	delete [] pop_mask;
	delete [] ns;
	delete [] types;
	delete [] pop_nsmpl;
	delete [] pop_cov;
	delete [] num_snps;
	delete [] td;
	delete [] fwh;
	for (i=0; i < npops; ++i)
		delete [] ncov[i];
	delete [] ncov;
}

void sfsData::calc_dw(void)
{
	int n, i;

	dw = new double* [sm->n+1];
	for (i=0; i <= sm->n; ++i)
		dw[i] = new double [sm->n+1]();

	for (n=2; n <= sm->n; ++n)
		for(i=n; i <= sm->n; ++i)
			dw[n][i] = (((2.0 * i * (n - i)) / (SQ(n-1))) - (1.0 / a1[n]));
}

void sfsData::calc_hw(void)
{
	int n, i;

	hw = new double* [sm->n+1];
	for (i=0; i <= sm->n; ++i)
		hw[i] = new double [sm->n+1]();

	for (n=2; n <= sm->n; ++n)
		for (i=n; i <= sm->n; ++i)
			hw[n][i] = ((1.0 / a1[n]) - ((double)(i) / (n - 1)));
}

void sfsData::calc_a1(void)
{
	int i, j;

	a1 = new double [sm->n+1];
	a1[0] = a1[1] = 1.0;

	// consider all sample sizes
	for (i=2; i <= sm->n; i++)
	{
		a1[i] = 0;
		for (j=1; j < i; j++)
			a1[i] += 1.0 / (double)(j);
	}
}

void sfsData::calc_a2(void)
{
	int i, j;

	a2 = new double [sm->n+2];
	a2[0] = a2[1] = 1.0;

	// consider all sample sizes
	for (i=2; i <= sm->n+1; i++)
	{
		a2[i] = 0;
		for (j=1; j < i; j++)
			a2[i] += 1.0 / (double)SQ(j);
	}
}

void sfsData::calc_e1(void)
{
	int i;
	double b1;

	e1 = new double [sm->n+1];
	e1[0] = e1[1] = 1.0;

	for (i=2; i <= sm->n; i++)
	{
		b1 = (i + 1.0) / (3.0 * (i-1));
		e1[i] = (b1 - (1.0 / a1[i])) / a1[i];
	}
}

void sfsData::calc_e2(void)
{
	int i;
	double b2;

	e2 = new double [sm->n+1];
	e2[0] = e2[1] = 1.0;

	for (i=2; i <= sm->n; i++)
	{
		b2 = (2.0 * (SQ(i) + i + 3.0)) / (9.0 * i * (i-1));
		e2[i] = (b2 - ((i + 2.0) / (a1[i] * i)) + (a2[i] / SQ(a1[i]))) / (SQ(a1[i]) + a2[i]);
	}
}

void sfsData::sfsUsage(void)
{
	std::cerr << std::endl;
	std::cerr << "Usage:   popbam sfs [options] <in.bam> [region]" << std::endl;
	std::cerr << std::endl;
	std::cerr << "Options: -i          base qualities are Illumina 1.3+               [ default: Sanger ]" << std::endl;
	std::cerr << "         -h  FILE    Input header file                              [ default: none ]" << std::endl;
	std::cerr << "         -w  INT     use sliding window of size (kb)" << std::endl;
	std::cerr << "         -p  STR     sample name of outgroup                        [ default: reference ]" << std::endl;
	std::cerr << "         -k  FLT     minimum proportion of sites covered in window  [ default: 0.5 ]" << std::endl;
	std::cerr << "         -n  FLT     minimum proportion of population covered       [ default: 1.0 ]" << std::endl;
	std::cerr << "         -f  FILE    Reference fastA file" << std::endl;
	std::cerr << "         -m  INT     minimum read coverage                          [ default: 3 ]" << std::endl;
	std::cerr << "         -x  INT     maximum read coverage                          [ default: 255 ]" << std::endl;
	std::cerr << "         -q  INT     minimum rms mapping quality                    [ default: 25 ]" << std::endl;
	std::cerr << "         -s  INT     minimum snp quality                            [ default: 25 ]" << std::endl;
	std::cerr << "         -a  INT     minimum map quality                            [ default: 13 ]" << std::endl;
	std::cerr << "         -b  INT     minimum base quality                           [ default: 13 ]" << std::endl;
	std::cerr << std::endl;
	exit(EXIT_FAILURE);
}
