#include "config.h"
#include "model.h"
#include "senderservice.h"
#include <boost/thread.hpp>
#include <boost/lexical_cast.hpp>
#include <Poco/Data/Session.h>
using namespace Poco::Data::Keywords;

// work around the fact that dcmtk doesn't work in unicode mode, so all string operation needs to be converted from/to mbcs
#ifdef _UNICODE
#undef _UNICODE
#undef UNICODE
#define _UNDEFINEDUNICODE
#endif

#include "dcmtk/config/osconfig.h"   /* make sure OS specific configuration is included first */
#include "dcmtk/dcmnet/diutil.h"
#include "dcmtk/oflog/ndc.h"

#ifdef _UNDEFINEDUNICODE
#define _UNICODE 1
#define UNICODE 1
#endif

SenderService::SenderService(CloudClient &cloudclient, DBPool &dbpool) :
cloudclient(cloudclient), dbpool(dbpool)
{
	shutdownEvent = false;
}

bool SenderService::getQueued(OutgoingSession &outgoingsession)
{
	try
	{
		Poco::Data::Session dbconnection(dbpool.get());

		Poco::Data::Statement queueselect(dbconnection);
		std::vector<OutgoingSession> sessions;
		queueselect << "SELECT id, uuid, queued, StudyInstanceUID, PatientName, PatientID, StudyDate, ModalitiesInStudy, destination_id, status,"
			"createdAt,updatedAt"
			" FROM outgoing_sessions WHERE queued = 1 LIMIT 1",
			into(sessions);

		queueselect.execute();

		if (sessions.size() > 0)
		{
			dbconnection << "UPDATE outgoing_sessions SET queued = 0 WHERE id = ? AND queued = 1", use(sessions[0].id), now;
			int rowcount = 0;
			dbconnection << "SELECT ROW_COUNT()", into(rowcount), now;

			if (rowcount > 0)
			{
				outgoingsession = sessions[0];
				return true;
			}

		}
	}
	catch (Poco::Exception &e)
	{
		std::string what = e.displayText();		
		DCMNET_ERROR(what);
	}

	return false;
}

void SenderService::run_internal()
{
	dcmtk::log4cplus::NDCContextCreator ndc("senderservice");
	while (!shouldShutdown())
	{
		OutgoingSession outgoingsession;
		if (getQueued(outgoingsession))
		{
			DCMNET_INFO("Session " << outgoingsession.uuid << " Sending study " << outgoingsession.StudyInstanceUID);

			Destination destination;
			if (findDestination(outgoingsession.destination_id, destination))
			{
				DCMNET_INFO(" to " << destination.destinationhost << ":" << destination.destinationport <<
					" destinationAE = " << destination.destinationAE << " sourceAE = " << destination.sourceAE);

				cloudclient.send_updateoutsessionitem(outgoingsession, destination.name);

				boost::shared_ptr<Sender> sender(new Sender(boost::lexical_cast<boost::uuids::uuid>(outgoingsession.uuid), cloudclient, dbpool));
				sender->Initialize(destination);

				naturalpathmap files;
				if (GetFilesToSend(outgoingsession.StudyInstanceUID, files))
				{
					sender->SetFileList(files);

					// start the thread
					sender->DoSendAsync();

					// save the object to the main list so we can track it
					senders.push_back(sender);
				}
			}
		}
		else
		{
#ifdef _WIN32
			Sleep(200);
#else
			usleep(200);
#endif
		}

		// look at the senders and delete any that's already done
		sharedptrlist::iterator itr = senders.begin();
		while (itr != senders.end())
		{

			if ((*itr)->IsDone())
				senders.erase(itr++);
			else
				itr++;
		}
	}

	// wait for all senders to be done, signal senders to cancel if they are not
	while (senders.size() > 0)
	{
		sharedptrlist::iterator itr = senders.begin();
		while (itr != senders.end())
		{
			if ((*itr)->IsDone())
				senders.erase(itr++);
			else
			{
				(*itr)->Cancel();
				itr++;
			}
		}
#ifdef _WIN32
		Sleep(200);
#else
		usleep(200);
#endif
	}
}

void SenderService::run()
{
	run_internal();
	dcmtk::log4cplus::threadCleanup();
}

void SenderService::stop()
{
	boost::mutex::scoped_lock lk(mutex);
	shutdownEvent = true;
}

bool SenderService::shouldShutdown()
{
	boost::mutex::scoped_lock lk(mutex);
	return shutdownEvent;
}


bool SenderService::findDestination(int id, Destination &destination)
{
	try
	{
		Poco::Data::Session dbconnection(dbpool.get());

		std::vector<Destination> dests;
		dbconnection << "SELECT id,"
			"name, destinationhost, destinationport, destinationAE, sourceAE,"
			"createdAt,updatedAt"
			" FROM destinations WHERE id = ? LIMIT 1",
			into(dests),
			use(id), now;

		if (dests.size() > 0)
		{
			destination = dests[0];
			return true;
		}
	}
	catch (...)
	{

	}
	return false;
}

bool SenderService::GetFilesToSend(std::string studyinstanceuid, naturalpathmap &result)
{
	try
	{
		// open the db
		Poco::Data::Session dbconnection(dbpool.get());

		std::vector<PatientStudy> patient_studies_list;

		Poco::Data::Statement patientstudiesselect(dbconnection);
		patientstudiesselect << "SELECT id,"
			"StudyInstanceUID,"
			"StudyID,"
			"AccessionNumber,"
			"PatientName,"
			"PatientID,"
			"StudyDate,"
			"ModalitiesInStudy,"
			"StudyDescription,"
			"PatientSex,"
			"PatientBirthDate,"
			"ReferringPhysicianName,"
			"NumberOfStudyRelatedInstances,"
			"createdAt,updatedAt"
			" FROM patient_studies WHERE StudyInstanceUID = ?",
			into(patient_studies_list),
			use(studyinstanceuid);

		patientstudiesselect.execute();
		
		if (patient_studies_list.size() <= 0)
		{
			throw std::exception();
		}

		std::vector<Series> series_list;
		Poco::Data::Statement seriesselect(dbconnection);
		seriesselect << "SELECT id,"
			"SeriesInstanceUID,"
			"Modality,"
			"SeriesDescription,"
			"SeriesNumber,"
			"SeriesDate,"
			"patient_study_id,"
			"createdAt,updatedAt"
			" FROM series WHERE patient_study_id = ?",
			into(series_list),
			use(patient_studies_list[0].id);

		seriesselect.execute();
		for (std::vector<Series>::iterator itr = series_list.begin(); itr != series_list.end(); itr++)
		{						
			std::vector<Instance> instance_list;
			Poco::Data::Statement instanceselect(dbconnection);
			instanceselect << "SELECT id,"
				"SOPInstanceUID,"
				"InstanceNumber,"
				"series_id,"
				"createdAt,updatedAt"
				" FROM instances WHERE series_id = ?",
				into(instance_list),
				use(itr->id);

			instanceselect.execute();

			boost::filesystem::path serieslocation;
			serieslocation = config::getStoragePath();
			serieslocation /= studyinstanceuid;
			serieslocation /= itr->SeriesInstanceUID;

			for (std::vector<Instance>::iterator itr2 = instance_list.begin(); itr2 != instance_list.end(); itr2++)
			{
				boost::filesystem::path filename = serieslocation;
				filename /= (*itr2).SOPInstanceUID + ".dcm";
				result.insert(std::pair<std::string, boost::filesystem::path>((*itr2).SOPInstanceUID, filename));				
			}
		}
	}
	catch (std::exception& e)
	{
		return false;
	}

	return true;
}

bool SenderService::cancelSend(std::string uuid)
{
	
	sharedptrlist::iterator itr = senders.begin();
	while (itr != senders.end())
	{
		if (itr->get()->isUUID(uuid))
		{
			itr->get()->Cancel();			
			return true;
		}
		itr++;
	}

	return false;
}