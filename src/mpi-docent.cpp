/*
 *  docent.cpp
 *
 *  Copyright 2012 by Christian Hardmeier. All rights reserved.
 *
 *  This file is part of Docent, a document-level decoder for phrase-based
 *  statistical machine translation.
 *
 *  Docent is free software: you can redistribute it and/or modify it under the
 *  terms of the GNU General Public License as published by the Free Software
 *  Foundation, either version 3 of the License, or (at your option) any later
 *  version.
 *
 *  Docent is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along with
 *  Docent. If not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <functional>
#include <iostream>
#include <iterator>
#include <vector>

#include <mpi.h>

#include <boost/foreach.hpp>
#include <boost/log/attributes/attribute.hpp>
#include <boost/log/attributes/attribute_values_view.hpp>
#include <boost/log/filters/basic_filters.hpp>
#include <boost/log/utility/init/common_attributes.hpp>
#include <boost/log/utility/init/to_console.hpp>
#include <boost/mpi/communicator.hpp>
#include <boost/mpi/nonblocking.hpp>
#include <boost/serialization/utility.hpp>
#include <boost/thread/thread.hpp>
#include <boost/unordered_map.hpp>

#include "Docent.h"
#include "DecoderConfiguration.h"
#include "DocumentState.h"
#include "MMAXDocument.h"
#include "NbestStorage.h"
#include "NistXmlTestset.h"
#include "Random.h"
#include "SimulatedAnnealing.h"

class DocumentDecoder {
private:
	typedef std::pair<uint,MMAXDocument> NumberedInputDocument;
	typedef std::pair<uint,PlainTextDocument> NumberedOutputDocument;

	static const int TAG_TRANSLATE = 0;
	static const int TAG_STOP_TRANSLATING = 1;
	static const int TAG_COLLECT = 2;
	static const int TAG_STOP_COLLECTING = 3;

	static Logger logger_;

	boost::mpi::communicator communicator_;
	DecoderConfiguration configuration_;

	static void manageTranslators(boost::mpi::communicator comm, NistXmlTestset &testset);

	PlainTextDocument runDecoder(const MMAXDocument &input);

public:
	DocumentDecoder(boost::mpi::communicator comm, const std::string &config) :
		communicator_(comm), configuration_(config) {}

	void runMaster(const std::string &infile);
	void translate();
};

Logger DocumentDecoder::logger_(logkw::channel = "DocumentDecoder");

class LogFilter : public std::unary_function<const boost::log::attribute_values_view &,bool> {
private:
	typedef boost::unordered_map<std::string,LogLevel> ChannelMapType_;
	ChannelMapType_ channelMap_;
	LogLevel defaultLevel_;

public:
	LogFilter() : defaultLevel_(normal) {}

	void setDefaultLevel(LogLevel deflt) {
		defaultLevel_ = deflt;
	}

	LogLevel &operator[](const std::string &channel) {
		return channelMap_[channel];
	}

	bool operator()(const boost::log::attribute_values_view &attrs) const {
		LogLevel level = *attrs["Severity"].extract<LogLevel>();
		ChannelMapType_::const_iterator it = channelMap_.find(*attrs["Channel"].extract<std::string>());
		if(it == channelMap_.end())
			return level >= defaultLevel_;
		else
			return level >= it->second;
	}
};

int main(int argc, char **argv) {
	int prov;
	MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &prov);
	std::cerr << "MPI implementation provides thread level " << prov << std::endl;

	boost::mpi::communicator world;

	LogFilter logFilter;
	//logFilter["WordSpaceCohesionModel"] = debug;
	logFilter["DocumentDecoder"] = debug;

	boost::log::init_log_to_console()->set_filter(boost::log::filters::wrap(logFilter));
	boost::log::add_common_attributes();

	if(argc != 3) {
		std::cerr << "Usage: docent config.xml input.xml" << std::endl;
		return 1;
	}

	DocumentDecoder decoder(world, argv[1]);

	if(world.rank() == 0)
		decoder.runMaster(argv[2]);
	else
		decoder.translate();

	MPI_Finalize();
	return 0;
}

void DocumentDecoder::runMaster(const std::string &infile) {
	NistXmlTestset testset(infile);

	boost::thread manager(manageTranslators, communicator_, testset);

	translate();

	manager.join();

	testset.outputTranslation(std::cout);
}

void DocumentDecoder::manageTranslators(boost::mpi::communicator comm, NistXmlTestset &testset) {
	namespace mpi = boost::mpi;

	mpi::request reqs[2];
	int stopped = 0;

	NumberedOutputDocument translation;
	reqs[0] = comm.irecv(mpi::any_source, TAG_COLLECT, translation);
	reqs[1] = comm.irecv(mpi::any_source, TAG_STOP_COLLECTING);

	NistXmlTestset::const_iterator it = testset.begin();
	uint docno = 0;
	for(int i = 0; i < comm.size() && it != testset.end(); ++i, ++docno, ++it) {
		BOOST_LOG_SEV(logger_, debug) << "S: Sending document " << docno << " to translator " << i;
		comm.send(i, TAG_TRANSLATE, std::make_pair(docno, *(*it)->asMMAXDocument()));
	}

	for(;;) {
		std::pair<mpi::status, mpi::request *> wstat = mpi::wait_any(reqs, reqs + 2);
		if(wstat.first.tag() == TAG_STOP_COLLECTING) {
			stopped++;
			BOOST_LOG_SEV(logger_, debug) << "C: Received STOP_COLLECTING from translator "
				<< wstat.first.source() << ", now " << stopped << " stopped translators.";
			if(stopped == comm.size()) {
				reqs[0].cancel();
				return;
			}
			*wstat.second = comm.irecv(mpi::any_source, TAG_STOP_COLLECTING);
		} else {
			BOOST_LOG_SEV(logger_, debug) << "C: Received translation of document " <<
				translation.first << " from translator " << wstat.first.source();
			reqs[0] = comm.irecv(mpi::any_source, TAG_COLLECT, translation);
			if(it != testset.end()) {
				BOOST_LOG_SEV(logger_, debug) << "S: Sending document " << docno <<
					" to translator " << wstat.first.source();
				comm.send(wstat.first.source(), TAG_TRANSLATE,
					std::make_pair(docno, *(*it)->asMMAXDocument()));
				++docno; ++it;
			} else {
				BOOST_LOG_SEV(logger_, debug) <<
					"S: Sending STOP_TRANSLATING to translator " << wstat.first.source();
				comm.send(wstat.first.source(), TAG_STOP_TRANSLATING);
			}
			testset[translation.first]->setTranslation(translation.second);
		}
	}
}

void DocumentDecoder::translate() {
	namespace mpi = boost::mpi;

	mpi::request reqs[2];
	reqs[1] = communicator_.irecv(0, TAG_STOP_TRANSLATING);
	NumberedInputDocument input;
	for(;;) {
		reqs[0] = communicator_.irecv(0, TAG_TRANSLATE, input);
		std::pair<mpi::status, mpi::request *> wstat = mpi::wait_any(reqs, reqs + 2);
		if(wstat.first.tag() == TAG_STOP_TRANSLATING) {
			BOOST_LOG_SEV(logger_, debug) << "T: Received STOP_TRANSLATING.";
			reqs[0].cancel();
			communicator_.send(0, TAG_STOP_COLLECTING);
			return;
		} else {
			NumberedOutputDocument output;
			BOOST_LOG_SEV(logger_, debug) << "T: Received document " << input.first << " for translation.";
			output.first = input.first;
			output.second = runDecoder(input.second);
			BOOST_LOG_SEV(logger_, debug) << "T: Sending translation of document " << input.first << " to collector.";
			communicator_.send(0, TAG_COLLECT, output);
		}
	}
}

PlainTextDocument DocumentDecoder::runDecoder(const MMAXDocument &input) {
	boost::shared_ptr<MMAXDocument> mmax = boost::make_shared<MMAXDocument>(input);
	boost::shared_ptr<DocumentState> doc(new DocumentState(configuration_, mmax));
	NbestStorage nbest(1);
	std::cerr << "Initial score: " << doc->getScore() << std::endl;
	configuration_.getSearchAlgorithm().search(doc, nbest);
	std::cerr << "Final score: " << doc->getScore() << std::endl;
	return doc->asPlainTextDocument();
}

std::ostream &operator<<(std::ostream &os, const std::vector<Word> &phrase) {
	bool first = true;
	BOOST_FOREACH(const Word &w, phrase) {
		if(!first)
			os << ' ';
		else
			first = false;
		os << w;
	}

	return os;
}

std::ostream &operator<<(std::ostream &os, const PhraseSegmentation &seg) {
	std::copy(seg.begin(), seg.end(), std::ostream_iterator<AnchoredPhrasePair>(os, "\n"));
	return os;
}

std::ostream &operator<<(std::ostream &os, const AnchoredPhrasePair &ppair) {
	os << ppair.first << "\t[" << ppair.second.get().getSourcePhrase().get() << "] -\t[" << ppair.second.get().getTargetPhrase().get() << ']';
	return os;
}

