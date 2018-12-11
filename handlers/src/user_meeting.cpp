#include <Poco/Data/SQLite/Connector.h>
#include <Poco/Data/Session.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <optional>
#include <mutex>

#include <loggers.hpp>
#include <handlers.hpp>
#include <user_meeting.hpp>
#include <sqlite.hpp>

namespace handlers {

using nlohmann::json;
using Poco::Data::Statement;
using Poco::Data::Keywords::into;
using Poco::Data::Keywords::now;
using Poco::Data::Keywords::range;
using Poco::Data::Keywords::use;

class SqliteStorage : public Storage {
public:
	void Save(Meeting &meeting) override {
		std::lock_guard<std::mutex> lock{m_mutex};
		if (meeting.id.has_value()) {
			Statement update(m_session);
			auto published = b2i(meeting.published);
			update << "UPDATE meeting SET "
				"name=?, description=?, address=?, published=? "
				"WHERE id=?",
				use(meeting.name),
				use(meeting.description),
				use(meeting.address),
				use(published),
				use(meeting.id.value()),
				now;

			m_count++;
			std::string msg = "executed query №" + std::to_string(m_count) + " update meeting with id = " + 
					std::to_string(meeting.id.value());
			Poco::Logger &logger = GetLoggers().getSqlLogger();
			logger.information(msg);
		} else {
			Statement insert(m_session);
			int published = b2i(meeting.published);
			insert << "INSERT INTO meeting (name, description, address, published) VALUES(?, ?, ?, ?)",
				use(meeting.name),
				use(meeting.description),
				use(meeting.address),
				use(published),
				now;

			Statement select(m_session);
			int id = 0;
			select << "SELECT last_insert_rowid()", into(id), now;
			meeting.id = id;

			m_count++;
			std::string msg = "executed query №" + std::to_string(m_count) + " insert meeting with id = " +
					 std::to_string(id);
			Poco::Logger &logger = GetLoggers().getSqlLogger();
			logger.information(msg);
		}
	}

	Storage::MeetingList GetList() override {
		std::lock_guard<std::mutex> lock{m_mutex};
		Storage::MeetingList list;
		Meeting meeting;
		Statement select(m_session);
		select << "SELECT id, name, description, address, published FROM meeting",
			into(meeting.id.emplace()),
			into(meeting.name),
			into(meeting.description),
			into(meeting.address),
			into(meeting.published),
			range(0, 1); //  iterate over result set one row at a time

		m_count++;
		Poco::Logger &logger = GetLoggers().getSqlLogger();
		logger.information("executed query №" + std::to_string(m_count) + " select meeting list");
		while (!select.done() && select.execute()) {
			list.push_back(meeting);
		}
		return list;
	}

	std::optional<Meeting> Get(int id) override {
		std::lock_guard<std::mutex> lock{m_mutex};
		int cnt = 0;
		m_session << "SELECT COUNT(*) FROM meeting WHERE id=?", use(id), into(cnt), now;
		if (cnt > 0) {
			Meeting meeting;
			Statement select(m_session);
			int tmp_id = 0;
			select << "SELECT id, name, description, address, published FROM meeting WHERE id=?",
				use(id),
				into(tmp_id),
				into(meeting.name),
				into(meeting.description),
				into(meeting.address),
				into(meeting.published),
				now;
			meeting.id = tmp_id;

			m_count++;
			std::string msg = "executed query №" + std::to_string(m_count) + " select meeting with id = " 
					+ std::to_string(id);
			Poco::Logger &logger = GetLoggers().getSqlLogger();
			logger.information(msg);

			return meeting;
		}
		return std::nullopt;
	}

	bool Delete(int id) override {
		std::lock_guard<std::mutex> lock{m_mutex};
		m_session << "DELETE FROM meeting WHERE id=?", use(id), now;

		m_count++;
		Poco::Logger &logger = GetLoggers().getSqlLogger();
		logger.information("executed query №" + std::to_string(m_count) + " delete meeting with id = " 
				+ std::to_string(id));

		return true;
	}

private:
	Poco::Data::Session m_session{sqlite::TYPE_SESSION, sqlite::DB_PATH};
	int m_count=0;
	std::mutex m_mutex;

	int b2i(bool b) {
		return b ? 1 : 0;
	}
};

Storage &GetStorage() {
	static SqliteStorage storage;
	return storage;
}

void UserMeetingList::HandleRestRequest(Poco::Net::HTTPServerRequest &/*request*/, Poco::Net::HTTPServerResponse &response) {
	response.setStatus(Poco::Net::HTTPServerResponse::HTTP_OK);

	auto &storage = GetStorage();
	nlohmann::json result = nlohmann::json::array();
	for (const auto &meeting : storage.GetList()) {
		result.push_back(meeting);
	}
	response.send() << result;

	Poco::Logger &logger = GetLoggers().getHttpResponseLogger();
	logger.information("sending response(code - HTTP_OK, body - meeting list)");
}

void UserMeetingCreate::HandleRestRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response) {
	response.setStatus(Poco::Net::HTTPServerResponse::HTTP_OK);
	nlohmann::json j = nlohmann::json::parse(request.stream());
	auto &storage = GetStorage();
	Meeting meeting = j;
	storage.Save(meeting);

	response.send() << json(meeting);

	Poco::Logger &logger = GetLoggers().getHttpResponseLogger();
	logger.information("sending response(code - HTTP_OK, body - new meeting)");
}

void UserMeetingRead::HandleRestRequest(Poco::Net::HTTPServerRequest &/*request*/, Poco::Net::HTTPServerResponse &response) {
	auto &meetings = GetStorage();
	auto meeting = meetings.Get(m_id);
	if (meeting.has_value()) {
		response.setStatusAndReason(Poco::Net::HTTPServerResponse::HTTP_OK);
		response.send() << json(meeting.value());

		Poco::Logger &logger = GetLoggers().getHttpResponseLogger();
		logger.information("sending response(code - HTTP_OK, body - meeting with id)");
		return;
	}

	response.setStatusAndReason(Poco::Net::HTTPServerResponse::HTTP_NOT_FOUND);
	response.send();

	Poco::Logger &logger = GetLoggers().getHttpResponseLogger();
	logger.information("sending response(code - HTTP_NOT_FOUND)");
}

void UserMeetingUpdate::HandleRestRequest(Poco::Net::HTTPServerRequest &request, Poco::Net::HTTPServerResponse &response) {
	response.setStatusAndReason(Poco::Net::HTTPServerResponse::HTTP_OK);
	auto &meetings = GetStorage();
	auto meeting = meetings.Get(m_id);
	if (meeting.has_value()) {
		auto body = nlohmann::json::parse(request.stream());
		Meeting new_meeting = body;
		new_meeting.id = m_id;
		meetings.Save(new_meeting);
		response.send() << json(new_meeting);

		Poco::Logger &logger = GetLoggers().getHttpResponseLogger();
		logger.information("sending response(code - HTTP_OK, body - updated meeting)");
		return;
	}

	response.setStatusAndReason(Poco::Net::HTTPServerResponse::HTTP_NOT_FOUND);
	response.send();

	Poco::Logger &logger = GetLoggers().getHttpResponseLogger();
	logger.information("sending response(code - HTTP_NOT_FOUND)");
}

void UserMeetingDelete::HandleRestRequest(Poco::Net::HTTPServerRequest &/*request*/, Poco::Net::HTTPServerResponse &response) {
	auto &meetings = GetStorage();
	Poco::Logger &logger = GetLoggers().getHttpResponseLogger();
	if (meetings.Delete(m_id)) {
		logger.information("sending response(code - HTTP_NO_CONTENT)");
		response.setStatusAndReason(Poco::Net::HTTPServerResponse::HTTP_NO_CONTENT);
	} else {
		logger.information("sending response(code - HTTP_NOT_FOUND)");
		response.setStatusAndReason(Poco::Net::HTTPServerResponse::HTTP_NOT_FOUND);
	}
	response.send();
}

} // namespace handlers
