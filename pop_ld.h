/** \file pop_ld.h
 *  \brief Header for the pop_ld.cpp file
 *  \author Daniel Garrigan
 *  \version 0.4
*/

#include "pop_base.h"

///
/// Additional include headers
///
#include <vector>
#include <algorithm>

///
/// Definitions
///

//
// Define data structures
//

/*!
 * \class ldData
 * \brief A derived class for passing parameters and data to the ld function
 */
class ldData: public popbamData
{
	public:
		// constructor
		ldData(const popbamOptions&);

		// destructor
		~ldData(void);

		// member variables
		int output;                             //!< Analysis output option
		unsigned int *pop_cov;                  //!< Boolean for population coverage
		int minSNPs;                            //!< Minimum number of snps for a window to be considered
		unsigned short minFreq;                 //!< Minimum allele count in LD calculation
		int *num_snps;                          //!< Number of SNPs in a given window
		double minSites;                        //!< Minimum proportion of aligned sites for a window to be considered
		double *omegamax;                       //!< Pointer to array of omega_max values
		double *wallb;                          //!< Pointer to array of Wall's B statistic
		double *wallq;                          //!< Pointer to array of Wall's Q statistic
		double *zns;                            //!< Pointer to array of ZnS values

		// member functions
		int calcZns(void);
		int calcOmegamax(void);
		int calcWall(void);
		int allocLD(void);
		int printLD(const std::string);
};

///
/// Function prototypes
///

/*!
* \fn unsigned long long *callBase(bam_sample_t *sm, errmod_t *em, int n, const bam_pileup1_t *pl)
* \brief Calls the base from the pileup at each position
* \param sm     Pointer to the sample data structure
* \param em     Pointer to the error model structure
* \param n      The number of reads in the pileup
* \param pl     Pointer to the pileup
* \return       Pointer to the consensus base call information for the individuals
*/
template unsigned long long* callBase<ldData>(ldData *t, int n, const bam_pileup1_t *pl);

/*!
 * \fn int make_ld(unsigned int tid, unsigned int pos, int n, const bam_pileup1_t *pl, void *data)
 * \brief Runs the linkage disequilibrium analysis
 * \param tid Chromosome identifier
 * \param pos Genomic position
 * \param n The read depth
 * \param pl A pointer to the alignment covering a single position
 * \param data A pointer to the user-passed data
 */
int makeLD(unsigned int tid, unsigned int pos, int n, const bam_pileup1_t *pl, void *data);

void usageLD(const std::string);

typedef int(ldData::*ld_func)(void);
