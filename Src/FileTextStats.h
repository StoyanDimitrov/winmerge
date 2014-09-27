/** 
 * @file FileTextStats.h
 *
 * @brief Declaration file for FileTextStats structure.
 */
// RCS ID line follows -- this is updated by CVS
// $Id$

#ifndef FileTextStats_h_included
#define FileTextStats_h_included

#define POCO_NO_UNWINDOWS 1
#include <Poco/Types.h>

/**
 * @brief Structure containing statistics about compared file.
 * This structure contains EOL-byte and zero-byte statistics from compared
 * file. Those statistics can be used to determine EOL style and binary
 * status of the file.
 */
struct FileTextStats
{
	unsigned ncrs; /**< Count of MAC (CR-byte) EOLs. */
	unsigned nlfs; /**< Count of Unix (LF-byte) EOLs. */
	unsigned ncrlfs; /**< Count of DOS (CR+LF-bytes) EOLs. */
	unsigned nzeros; /**< Count of zero-bytes. */
	FileTextStats() { clear(); }
	void clear() { ncrs = nlfs = ncrlfs = nzeros = 0; }
};


#endif // FileTextStats_h_included