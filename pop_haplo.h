/** \file pop_haplo.h
 *  \brief Header for the pop_haplo.cpp file
 *  \author Daniel Garrigan
 *  \version 0.4
*/

#include "pop_base.h"

///
/// Additional include headers
///
#include <limits>
#include <algorithm>
#include <vector>
#include <list>
#include <set>

///
/// Definitions
///

//
// Define data structures
//

/*!
 * \class haploData
 * \brief A derived class for passing parameters and data to the haplo function
 */
class haploData: public popbamData
{
	public:
		// constructor
		haploData(const popbamOptions&);

		// destructor
		~haploData(void);

		// member public variables
		double minSites;                            //!< User-specified minimum number of aligned sites to perform analysis
		double minPop;                              //!< Minimum proportion of samples present
		unsigned int *nsite_matrix;                 //!< Matrix of pairwise aligned sites
		unsigned int *diff_matrix;                  //!< Matrix of pairwise sequence difference
		unsigned int *minDxy;                       //!< Array of minimum between-population Dxy
		std::vector<std::vector<std::string>> hap;  //!< Vector of strings to hold the haplotypes

		// member public functions
		int allocHaplo(void);
		int calcHaplo(void);
		int printHaplo(const std::string);

	private:
		// member private variables
		int output;                             //!< Analysis output option
		int *nhaps;                             //!< Array of number of haplotypes within populations
		double *hdiv;                           //!< Array of haplotype diversities within populations
		double *piw;                            //!< Array of within-population heterozygosities
		double *pib;                            //!< Array of between-population heterozygosities
		double *ehhs;                           //!< Array of site-specfic haplotype homozygosity statistics within populations

		// member private function
		int calcNhaps(void);
		int calcEHHS(void);
		int calcGmin(void);
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
template unsigned long long* callBase<haploData>(haploData *t, int n, const bam_pileup1_t *pl);

/*!
 * \fn int make_haplo(unsigned int tid, unsigned int pos, int n, const bam_pileup1_t *pl, void *data)
 * \brief Calculate haplotype-based statistics
 * \param tid Chromosome identifier
 * \param pos Genomic position
 * \param n The read depth
 * \param pl A pointer to the alignment covering a single position
 * \param data A pointer to the user-passed data
 */
int makeHaplo(unsigned int tid, unsigned int pos, int n, const bam_pileup1_t *pl, void *data);

void usageHaplo(const std::string);

typedef int(haploData::*haplo_func)(void);
