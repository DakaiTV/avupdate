#include "updater_impl.hpp"
#include "uncompress.hpp"
#ifdef WIN32
#include <windows.h>
#endif // WIN32

updater_impl::updater_impl(void)
{
	m_result = updater::st_error;
}

updater_impl::~updater_impl(void)
{
	stop();
}

bool updater_impl::start(const std::string& url, fun_check_files_callback fc, fun_down_load_callback dl,
	fun_check_files_callback cf, fun_update_files_process uf, std::string setup_path)
{
	m_setup_file_check = fc;
	m_down_load_fun = dl;
	m_fun_check_files = cf;
	m_update_files_fun = uf;
	m_url = url;
	m_upfile_total_size = 0;
	m_current_index = 0;
	m_total_read_bytes = 0;
	m_setup_path = setup_path;
	m_is_downloading = false;
	m_abort = false;
	m_paused = false;
	m_result = updater::st_updating;
	m_need_update_list.clear();
	m_update_file_list.clear();

	if (url.empty())
		return false;
	m_update_thrd = boost::thread(boost::bind(&updater_impl::update_files, this));

	return true;
}

void updater_impl::stop()
{
	if (m_abort)
		return ;
	m_abort = true;
	boost::shared_ptr<tcp::socket> sock_ptr = m_sock.lock();
	if (sock_ptr) {
		boost::system::error_code ec;
		sock_ptr->cancel(ec);
		sock_ptr->shutdown(tcp::socket::shutdown_both, ec);
		sock_ptr->close(ec);
	}

	m_update_thrd.join();
}

void updater_impl::pause()
{
	m_paused = true;
}

void updater_impl::resume()
{
	m_paused = false;
}

bool updater_impl::check_update(const std::string& l, const std::string& s)
{
	try
	{
		std::string file_name;
		std::string target_file;
		char md5_buf[33] = { 0 };
		url u = l;

		// 得到临时文件夹路径.
		fs::path p = fs::temp_directory_path();
		file_name = p.string() + u.filename();
		std::string extera_header = make_http_last_modified(file_name);
		xml_node_info info;
		// 下载xml文件到临时文件夹.
		m_current_index = 0;
		if (file_down_load(l, file_name, info, extera_header)) {
			// 解析xml文件.
			if (!parser_xml_file(file_name)) {
				std::cout << "parser xml file failed!\n";
				m_result = updater::st_error;
				return false;
			}
			// 检查xml和安装目录下的文件, 得到需要更新的文件列表.
			for (std::map<std::string, xml_node_info>::iterator i = m_update_file_list.begin();
				i != m_update_file_list.end(); i++) {
					// 只做检查时不回调.
					// if (m_setup_file_check)
					// 	m_setup_file_check(i->first, m_update_file_list.size(), m_current_index++);
					fs::path p = fs::path(s) / i->second.name;
					if (!fs::exists(p)) {
						m_need_update_list.insert(std::make_pair(i->first, i->second));
						m_upfile_total_size += i->second.size;
						continue;
					}
					std::string md5;
					memset(md5_buf, 0, 33);
					MDFile((char*)p.string().c_str(), md5_buf);
					md5 = md5_buf;
					boost::to_lower(md5);
					if (!i->second.check && (md5 != i->second.md5 || i->second.md5.empty())) {
						m_need_update_list.insert(std::make_pair(i->first, i->second));
						m_upfile_total_size += i->second.size;
					}
			}
		}
	}
	catch (std::exception& e) {
		std::cout << "exception: " << e.what() << std::endl;
	}

	// 如果更新列表为空, 则表示不需要更新.
	if (m_need_update_list.size() == 0)
		return false;

	return true;
}

void updater_impl::update_files()
{
	bool is_need_rollback = false;
	try
	{
		std::string file_name;
		std::string target_file;
		char md5_buf[33] = { 0 };
		url u = m_url;

		// 得到临时文件夹路径.
		fs::path p = fs::temp_directory_path();
		file_name = p.string() + u.filename();
		std::string extera_header = make_http_last_modified(file_name);
		xml_node_info info;
		// 下载xml文件到临时文件夹.
		m_current_index = 0;
		m_upfile_total_size = 0;
		if (file_down_load(m_url, file_name, info, extera_header)) {
			// 解析xml文件.
			if (!parser_xml_file(file_name)) {
				std::cout << "parser xml file failed!\n";
				m_result = updater::st_error;
				return ;
			}
			// 得到需要更新的文件列表.
			for (std::map<std::string, xml_node_info>::iterator i = m_update_file_list.begin();
				i != m_update_file_list.end(); i++) {
					if (m_setup_file_check)
						m_setup_file_check(i->first, m_update_file_list.size(), m_current_index++);
				fs::path p = fs::path(m_setup_path) / i->second.name;
				if (!fs::exists(p)) {
					m_need_update_list.insert(std::make_pair(i->first, i->second));
					m_upfile_total_size += i->second.size;
					continue;
				}
				std::string md5;
				memset(md5_buf, 0, 33);
				MDFile((char*)p.string().c_str(), md5_buf);
				md5 = md5_buf;
				boost::to_lower(md5);
				if (!i->second.check && (md5 != i->second.md5 || i->second.md5.empty())) {
					m_need_update_list.insert(std::make_pair(i->first, i->second));
					m_upfile_total_size += i->second.size;
				}
			}
			extera_header = "";
			m_is_downloading = true;
			m_current_index = 0;
			// 根据xml下载各文件.
			for (std::map<std::string, xml_node_info>::iterator i = m_need_update_list.begin();
				i != m_need_update_list.end(); i++) {
					if (m_abort)
						return ;
					if (m_paused) {
						while (m_paused)
							boost::this_thread::sleep(boost::posix_time::millisec(100));
					}
					u = i->second.url;
					fs::path temp_path = p.string() + fs::path(i->first).parent_path().string();
					temp_path = temp_path / u.filename();
					file_name = temp_path.string();
					// 此处验证压缩文件的md5值, 根据md5进行判断是否下载.
					std::string md5;
					if (fs::exists(file_name)) {
						char md5_buf[33] = { 0 };
						MDFile((char*)file_name.c_str(), md5_buf);
						md5 = md5_buf;
						boost::to_lower(md5);
					}
					// 比较md5.
					if (md5 != i->second.filehash || i->second.filehash == "") {
						if (!file_down_load(i->second.url, file_name, i->second, extera_header)) {
							std::cout << "download file \'" << file_name.c_str() << "\'failed!\n";
							return ;
						}
					} else {
						m_total_read_bytes += i->second.size;
					}
					m_current_index++;
			}
			m_is_downloading = false;
			m_current_index = 0;
			// 解压并检查下载文件的md5值.
			for (std::map<std::string, xml_node_info>::iterator i = m_need_update_list.begin();
				i != m_need_update_list.end(); i++) {
					if (m_abort)
						return ;
					if (m_paused) {
						while (m_paused) 
							boost::this_thread::sleep(boost::posix_time::millisec(100));
					}
					u = i->second.url;
					fs::path temp_path = p.string() + fs::path(i->first).parent_path().string();
					temp_path = temp_path / u.filename();
					file_name = temp_path.string();
					// 解压缩, 支持gz和zip两种压缩方案.
					if (!i->second.compress.empty()) {
						if (i->second.compress == "gz") {
							if (do_extract_gz(file_name.c_str()) != 0) {
								std::cout << "extract gz file \'" << file_name.c_str() << "\'failed!\n";
								m_result = updater::st_error;
								return ;
							}
						} else if (i->second.compress == "zip") {
							std::string str_path = (temp_path.parent_path() / "./").string();
							if (do_extract_zip(file_name.c_str(), str_path.c_str()) != 0) {
								std::cout << "extract zip file \'" << file_name.c_str() << "\'failed!\n";
								m_result = updater::st_error;
								return ;
							}
						}
					}
					// 计算md5.
					temp_path = p / i->first;
					file_name = temp_path.string();
					char md5_buf[33] = { 0 };
					MDFile((char*)file_name.c_str(), md5_buf);
					std::string md5 = md5_buf;
					boost::to_lower(md5);
					// 比较md5.
					if (md5 != i->second.md5) {
						std::cout << "download file md5 check failed, xml md5:\'" << i->second.md5.c_str()
							<< "\' current file md5:\'" << md5.c_str() << "\'\n";
						return ;
					}
					if (m_fun_check_files)
						m_fun_check_files(i->first, m_need_update_list.size(), m_current_index++);
			}
			// 复制安装.
			m_current_index = 0;
			is_need_rollback = true;
			for (std::map<std::string, xml_node_info>::iterator i = m_need_update_list.begin();
				i != m_need_update_list.end(); i++) {
					if (m_abort)
						throw std::exception("user abort!"); // for rollback.
					if (m_paused) {
						while (m_paused) 
							boost::this_thread::sleep(boost::posix_time::millisec(100));
					}
					file_name = (p / i->first).string();
					target_file = (fs::path(m_setup_path) / i->first).string();
					if (!fs::exists(fs::path(target_file).parent_path())) {
						create_directories(fs::path(target_file).parent_path());
					}
					if (fs::exists(fs::path(target_file))) {
						char md5_buf[33] = { 0 };
						MDFile((char*)target_file.c_str(), md5_buf);
						std::string md5 = md5_buf;
						boost::to_lower(md5);
						if (md5 != i->second.md5 && !i->second.check) {
							fs::rename(fs::path(target_file), fs::path(target_file + ".bak"));
							fs::copy_file(fs::path(file_name), fs::path(target_file), fs::copy_option::overwrite_if_exists);
						}
					} else {
						fs::copy_file(fs::path(file_name), fs::path(target_file), fs::copy_option::overwrite_if_exists);
					}
					if (!i->second.command.empty()) {
					#ifdef WIN32
						WinExec(i->second.command.c_str(), SW_HIDE);
					#else
						system(i->second.command.c_str());
					#endif // WIN32
					}
					if (m_update_files_fun)
						m_update_files_fun(i->first, m_need_update_list.size(), m_current_index++);
			}
			// 清理bak文件.
			is_need_rollback = false;
			for (std::map<std::string, xml_node_info>::iterator i = m_update_file_list.begin();
				i != m_update_file_list.end(); i++) {
					if (m_abort)
						return ;
					if (m_paused) {
						while (m_paused) 
							boost::this_thread::sleep(boost::posix_time::millisec(100));
					}
					// 执行命令.
					if (i->second.command == "regsvr") {
						std::string cmd = "regsvr32.exe /s " + (fs::path(m_setup_path) / i->first).string();
					#ifdef WIN32
						WinExec(cmd.c_str(), SW_HIDE);
					#else
						system(cmd.c_str());
					#endif // WIN32
					} else if (i->second.command != "") {
					#ifdef WIN32
						WinExec(i->second.command.c_str(), SW_HIDE);
					#else
						system(i->second.command.c_str());
					#endif // WIN32
					}
					target_file = (fs::path(m_setup_path) / i->first).string() + ".bak";
					if (fs::exists(fs::path(target_file))) {
						boost::system::error_code ec;
						remove(fs::path(target_file), ec); // 忽略错误.
					}
			}
			// 修改更新结果状态.
			if (m_need_update_list.size() == 0)
				m_result = updater::st_no_need_update;
			else
				m_result = updater::st_succeed;
		} else {
			std::cout << "download xml file failed!\n";
			return ;
		}
	}
	catch (std::exception& e)
	{
		std::cout << "exception: " << e.what() << std::endl;
		m_result = updater::st_error;
		if (is_need_rollback) {
			// 回滚操作.
			for (std::map<std::string, xml_node_info>::iterator i = m_update_file_list.begin();
				i != m_update_file_list.end(); i++) {
					std::string target_file = (fs::path(m_setup_path) / i->first).string();
					std::string backup_file = fs::path(target_file + ".bak").string();
					if (!fs::exists(fs::path(target_file)) && fs::exists(fs::path(backup_file)))
						rename(fs::path(target_file + ".bak"), fs::path(target_file));
			}
		}
	}
}

bool updater_impl::file_down_load(const std::string& u, const std::string& file, 
	xml_node_info& info, const std::string& extera_header/* = ""*/)
{
	time_t last_modified_time = 0;
	url url = u;

	try {
		std::fstream fs;

		// 创建目录.
		fs::path p = fs::path(file).parent_path();
		if (!fs::exists(p)) {
			if (!create_directories(p))
				return false;
		}

		boost::asio::io_service &io_service = m_io_service;
		tcp::resolver resolver(io_service);
		char buffer[4096] = { 0 };
		sprintf(buffer, "%d", url.port());
		tcp::resolver::query query(url.host().c_str(), buffer);
		tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);
		tcp::resolver::iterator end;

		boost::shared_ptr<tcp::socket> sock_ptr(new tcp::socket(io_service));
		m_sock = sock_ptr;
		tcp::socket &socket = *sock_ptr.get();

		boost::system::error_code error = boost::asio::error::host_not_found;
		while (error && endpoint_iterator != end) {
			socket.close();
			socket.connect(*endpoint_iterator++, error);
		}

		// 连接失败.
		if (error) {
			m_result = updater::st_unable_to_connect;
			return false;
		}

		boost::asio::streambuf request;
		std::ostream request_stream(&request);

		request_stream << "GET " << u << " HTTP/1.0\r\n";
		request_stream << "Host: " << url.host() << "\r\n";
		request_stream << "Accept: */*\r\n";
		if (!extera_header.empty())
			request_stream << extera_header.c_str();
		request_stream << "Connection: close\r\n\r\n";

		boost::asio::write(socket, request);

		boost::asio::streambuf response;
		boost::asio::read_until(socket, response, "\r\n");
		std::istream response_stream(&response);
		std::string http_version;
		response_stream >> http_version;
		unsigned int status_code;
		response_stream >> status_code;
		std::string status_message;
		std::getline(response_stream, status_message);

		if (!response_stream || http_version.substr(0, 5) != "HTTP/") {
			std::cout << "Invalid response\n";
			m_result = updater::st_invalid_http_response;
			return false;
		}
		std::cout << "Response returned with status code " << status_code << "\n";
		if (status_code != 200) {
			if (status_code == 304) {
				if (m_is_downloading) {
					std::map<std::string, xml_node_info>::iterator finder = 
						m_need_update_list.find(info.name);
					if (finder != m_need_update_list.end()) {
						m_total_read_bytes += finder->second.size;
						down_load_callback(url.filename(), m_need_update_list.size(), m_current_index, 
							m_upfile_total_size, m_total_read_bytes, finder->second.size, finder->second.size);
					}
				}
				goto SUCCESS_FLAG;
			}
			m_result = updater::st_invalid_http_response;
			return false;
		}

		// Read the response headers, which are terminated by a blank line.
		boost::asio::read_until(socket, response, "\r\n\r\n");

		// Process the response headers.
		int recvive_bytes = 0;
		int content_length = 0;
		int remainder = 0;
		std::string header;
		std::string modified_time;

		while (std::getline(response_stream, header) && header != "\r") {
			boost::regex ex;
			boost::smatch what;
			std::string::const_iterator start, end;

			start = header.begin();
			end = header.end();

			// match Content-Length
			ex.assign("Content-Length.*?\\:.*?(\\d+)");
			if (boost::regex_search(start, end, what, ex, boost::match_default))
				content_length = atoi(std::string(what[1]).c_str());
			ex.assign("Last-Modified.*?\\:\\s?(\\w+),\\s?(\\d+)\\s?(\\w+)\\s?(\\d+)\\s?(\\d+)\\:(\\d+)\\:(\\d+)");
			if (boost::regex_search(start, end, what, ex, boost::match_default)) {
				struct tm date = { 0 };
				if (parser_http_last_modified(header, &date))
					last_modified_time = mktime(&date);
			}
		}

		remainder = content_length;

		// 创建文件.
		fs.open(file.c_str(), std::ios::trunc | std::ios::in | std::ios::out | std::ios::binary);
		if (fs.bad() || fs.fail()) {
			m_result = updater::st_open_file_failed;
			return false;
		}

		// Write whatever content we already have to output.
		while (response.size() > 0) {
			recvive_bytes = response.size() > 4096 ? 4096 : response.size();
			response.sgetn(buffer, recvive_bytes);
			fs.write(buffer, recvive_bytes);
			remainder -= recvive_bytes;
		}

		// Call callback function.
		if (m_is_downloading) {
			int read_bytes = content_length - remainder;
			m_total_read_bytes += read_bytes;
			down_load_callback(url.filename(), m_update_file_list.size(), m_current_index, 
				m_upfile_total_size, m_total_read_bytes, content_length, read_bytes);
		}

		if (remainder <= 0)
			goto SUCCESS_FLAG;

		// Read until EOF, writing data to output as we go.
		while (boost::asio::read(socket, response,
			boost::asio::transfer_at_least(1), error))
		{
			while (response.size() > 0) {
				recvive_bytes = response.size() > 4096 ? 4096 : response.size();
				response.sgetn(buffer, recvive_bytes);
				fs.write(buffer, recvive_bytes);
				remainder -= recvive_bytes;
				if (m_is_downloading) {
					int read_bytes = content_length - remainder;
					m_total_read_bytes += recvive_bytes;
					down_load_callback(url.filename(), m_update_file_list.size(), m_current_index, 
						m_upfile_total_size, m_total_read_bytes, content_length, read_bytes);
				}
				if (m_abort)
					return false;
				if (remainder <= 0) {
					fs.close();
					goto SUCCESS_FLAG;
				}
			}
		}

		if (error != boost::asio::error::eof)
			return false;

		fs.close();
		goto SUCCESS_FLAG;
	} catch (std::exception& e) {
		std::cout << e.what() << std::endl;
		return false;
	}

SUCCESS_FLAG:

	if (last_modified_time != 0) {
		fs::path p(file);
		if (fs::exists(p))
			last_write_time(p, last_modified_time);
	}

	return true;
}

bool updater_impl::file_down_load_by_avhttp(const std::string& u,
	const std::string& file, xml_node_info& info, const std::string& extera_header /*= ""*/)
{
	return false;
}

void updater_impl::down_load_callback(std::string file, int count, int index, 
	int total_size, int total_read_bytes, int file_size, int read_bytes)
{
	if (!m_is_downloading)
		return ;

	if (m_upfile_total_size == -1)
		return ;

	if (m_abort)
		return ;

	if (m_down_load_fun) {
		m_down_load_fun(file, count, index, 
			total_size, total_read_bytes, file_size, read_bytes);
	}
}

bool updater_impl::parser_xml_file(const std::string& file)
{
	TiXmlNode* node = 0;
	TiXmlElement* element = 0;
	TiXmlDocument doc(file.c_str());

	if (!doc.LoadFile())
		return false;

	node = doc.FirstChild("update_root");

	if (node) {
		element = node->ToElement();
		int sum = atol(element->Attribute("count"));
		int count = 0;

		node = node->FirstChild();

		while (node) {
			count++;
			element = node->ToElement();
			if (element) {
				char* item = NULL;
				xml_node_info xml = { std::string(""), std::string(""), std::string(""), 
					std::string(""), std::string(""), std::string(""), false, 0 };

				if (element->Attribute("name"))
					xml.name = element->Attribute("name");
				if (element->Attribute("md5"))
					xml.md5 = element->Attribute("md5");
				boost::to_lower(xml.md5);
				if (element->Attribute("filehash"))
					xml.filehash = element->Attribute("filehash");
				boost::to_lower(xml.filehash);
				if (element->Attribute("url"))
					xml.url = element->Attribute("url");
				if (element->Attribute("command"))
					xml.command = element->Attribute("command");
				if (element->Attribute("compress"))
					xml.compress = element->Attribute("compress");
				if (element->Attribute("check"))
					xml.check = true;
				if (element->Attribute("size")) {
					xml.size = atol(element->Attribute("size"));
				}

				m_update_file_list.insert(std::make_pair(xml.name, xml));
			}
			node = node->NextSibling();
		}

#ifdef _DEBUG
		for (std::map<std::string, xml_node_info>::iterator i = m_update_file_list.begin();
			i != m_update_file_list.end(); i++) {
				std::cout << i->first.c_str() << "\n";
		}
#endif // _DEBUG

		if (count == sum)
			return true;
	}

	return false;
}

updater::result_type updater_impl::result()
{
	return m_result;
}


updater::updater()
	: m_updater(new updater_impl())
{}

updater::~updater()
{
	delete m_updater;	
}

bool updater::start(const std::string& url, fun_check_files_callback fc, fun_down_load_callback dl,
	fun_check_files_callback cf, fun_update_files_process uf, std::string setup_path)
{
	return m_updater->start(url, fc, dl, cf, uf, setup_path);
}

void updater::stop()
{
	m_updater->stop();
}

void updater::pause()
{
	m_updater->pause();
}

void updater::resume()
{
	m_updater->resume();
}

updater::result_type updater::result()
{
	return m_updater->result();
}

bool updater::check_update(const std::string& url, const std::string& setup_path)
{
	return m_updater->check_update(url, setup_path);
}





