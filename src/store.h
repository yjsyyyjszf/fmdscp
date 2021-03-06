#ifndef __STORE_H__
#define __STORE_H__

#include <boost/filesystem.hpp>
#include "dbpool.h"
#include "model.h"

// work around the fact that dcmtk doesn't work in unicode mode, so all string operation needs to be converted from/to mbcs
#ifdef _UNICODE
#undef _UNICODE
#undef UNICODE
#define _UNDEFINEDUNICODE
#endif

#include "dcmtk/config/osconfig.h"   /* make sure OS specific configuration is included first */
#include "dcmtk/ofstd/ofstd.h"
#include "dcmtk/dcmdata/dctk.h"

#ifdef _UNDEFINEDUNICODE
#define _UNICODE 1
#define UNICODE 1
#endif

class StoreHandler
{

public:	
	StoreHandler(DBPool &dbpool) : dbpool(dbpool) {};
	Uint16 handleSTORERequest(boost::filesystem::path filename);
	bool AddDICOMFileInfoToDatabase(DcmFileFormat &dfile);
	OFCondition UploadToS3(boost::filesystem::path filename, std::string sopuid, std::string seriesuid, std::string studyuid);

	void notifyAssociationEnd();
protected:
	void UpdateStudyInstanceCount(std::string StudyInstanceUID, Poco::Data::Session &dbconnection);

	PatientStudy patientstudy;
	Series series;


	DBPool &dbpool;
};

#endif