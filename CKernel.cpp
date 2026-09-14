#include "CKernel.h"
#include "common/Logger.h"
#include"mediator/TcpServerMediator.h"
#include<cstring>
#include "net/platform_compat.h"
#include "common/ServerMetrics.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <array>
#include <iomanip>
#include <sstream>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <algorithm>

namespace {
constexpr int kPasswordIterations = 120000;
constexpr std::size_t kPasswordSaltSize = 16;
constexpr std::size_t kPasswordHashSize = 32;
constexpr const char* kPasswordPrefix = "pbkdf2_sha256$";
constexpr int kMaxLoginFailures = 5;
constexpr auto kLoginFailureWindow = std::chrono::minutes(5);
constexpr auto kLoginBlockDuration = std::chrono::seconds(60);
constexpr auto kLoginAttemptRetention = std::chrono::minutes(15);
constexpr std::size_t kMaxLoginAttemptEntries = 4096;

class SensitiveBufferCleaner
{
public:
	SensitiveBufferCleaner(void* data, std::size_t length)
		: m_data(data), m_length(length) {}
	~SensitiveBufferCleaner()
	{
		if (m_data != nullptr && m_length > 0) OPENSSL_cleanse(m_data, m_length);
	}
	SensitiveBufferCleaner(const SensitiveBufferCleaner&) = delete;
	SensitiveBufferCleaner& operator=(const SensitiveBufferCleaner&) = delete;
private:
	void* m_data;
	std::size_t m_length;
};

// 将二进制数据转换为小写十六进制，便于安全地存入文本字段。
std::string bytesToHex(const unsigned char* data, std::size_t length)
{
	std::ostringstream stream;
	stream << std::hex << std::setfill('0');
	for (std::size_t index = 0; index < length; ++index) {
		stream << std::setw(2) << static_cast<int>(data[index]);
	}
	return stream.str();
}

// 使用 OpenSSL 计算 SHA-256，并转换为十六进制字符串。
static bool sha256Hex(
	const char* source,
	std::string& result)
{
	if (source == nullptr) {
		return false;
	}

	unsigned char digest[SHA256_DIGEST_LENGTH];
	if (!SHA256(reinterpret_cast<const unsigned char*>(source), strlen(source), digest)) return false;
	std::ostringstream stream;
	stream << std::hex << std::setfill('0');
	for (unsigned char value : digest) stream << std::setw(2) << static_cast<int>(value);
	result = stream.str();
	return true;
}

bool hexToBytes(const std::string& text, std::vector<unsigned char>& bytes)
{
	if (text.empty() || text.size() % 2 != 0) return false;
	bytes.clear();
	bytes.reserve(text.size() / 2);
	for (std::size_t index = 0; index < text.size(); index += 2) {
		char* end = nullptr;
		const std::string pair = text.substr(index, 2);
		const unsigned long value = std::strtoul(pair.c_str(), &end, 16);
		if (end == pair.c_str() || *end != '\0' || value > 255) return false;
		bytes.push_back(static_cast<unsigned char>(value));
	}
	return true;
}

bool derivePassword(
	const char* password,
	const unsigned char* salt,
	std::size_t saltLength,
	int iterations,
	std::array<unsigned char, kPasswordHashSize>& result)
{
	return password != nullptr && salt != nullptr && iterations > 0 &&
		PKCS5_PBKDF2_HMAC(
			password,
			-1,
			salt,
			static_cast<int>(saltLength),
			iterations,
			EVP_sha256(),
			static_cast<int>(result.size()),
			result.data()) == 1;
}

// 新密码使用随机盐和多轮PBKDF2，抵抗数据库泄露后的离线暴力破解。
bool passwordForStorage(const char* password, std::string& encoded)
{
	std::array<unsigned char, kPasswordSaltSize> salt{};
	std::array<unsigned char, kPasswordHashSize> hash{};
	if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
		!derivePassword(password, salt.data(), salt.size(), kPasswordIterations, hash)) {
		return false;
	}
	encoded = std::string(kPasswordPrefix) + std::to_string(kPasswordIterations) + "$" +
		bytesToHex(salt.data(), salt.size()) + "$" + bytesToHex(hash.data(), hash.size());
	return true;
}

// 同时兼容旧版64位SHA-256；旧格式验证成功后由登录流程自动升级。
bool verifyPassword(const char* password, const std::string& stored, bool& needsUpgrade)
{
	needsUpgrade = false;
	if (password == nullptr) return false;

	if (stored.rfind(kPasswordPrefix, 0) != 0) {
		std::string legacyHash;
		if (stored.size() != SHA256_DIGEST_LENGTH * 2 ||
			!sha256Hex(password, legacyHash)) return false;
		needsUpgrade = true;
		return CRYPTO_memcmp(legacyHash.data(), stored.data(), stored.size()) == 0;
	}

	const std::size_t iterationStart = std::strlen(kPasswordPrefix);
	const std::size_t saltSeparator = stored.find('$', iterationStart);
	const std::size_t hashSeparator = saltSeparator == std::string::npos
		? std::string::npos : stored.find('$', saltSeparator + 1);
	if (saltSeparator == std::string::npos || hashSeparator == std::string::npos) return false;

	const std::string iterationText = stored.substr(
		iterationStart, saltSeparator - iterationStart);
	char* end = nullptr;
	const unsigned long parsedIterations = std::strtoul(iterationText.c_str(), &end, 10);
	if (end == iterationText.c_str() || *end != '\0' ||
		parsedIterations < 10000 || parsedIterations > 1000000) return false;

	std::vector<unsigned char> salt;
	std::vector<unsigned char> expected;
	if (!hexToBytes(stored.substr(saltSeparator + 1, hashSeparator - saltSeparator - 1), salt) ||
		!hexToBytes(stored.substr(hashSeparator + 1), expected) ||
		salt.empty() || expected.size() != kPasswordHashSize) return false;

	std::array<unsigned char, kPasswordHashSize> actual{};
	if (!derivePassword(password, salt.data(), salt.size(),
		static_cast<int>(parsedIterations), actual)) return false;
	return CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0;
}
}
CKernel* CKernel::pKernel=nullptr;
CKernel::CKernel():m_pMediator (nullptr){
	pKernel = this;
	setProtocol();
}
CKernel::~CKernel(){}

bool CKernel::isLoginAllowed(const std::string& accountKey, int& retryAfterSeconds)
{
	retryAfterSeconds = 0;
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(m_loginAttemptsMutex);

	// 达到容量时顺便淘汰长期未访问项，限制恶意随机账号造成的内存增长。
	if (m_loginAttempts.size() >= kMaxLoginAttemptEntries) {
		for (auto it = m_loginAttempts.begin(); it != m_loginAttempts.end();) {
			if (now - it->second.lastSeen > kLoginAttemptRetention) {
				it = m_loginAttempts.erase(it);
			} else {
				++it;
			}
		}
	}

	auto it = m_loginAttempts.find(accountKey);
	if (it == m_loginAttempts.end()) return true;
	if (it->second.blockedUntil == std::chrono::steady_clock::time_point{} &&
		now - it->second.lastSeen > kLoginFailureWindow) {
		it->second.failures = 0;
	}
	it->second.lastSeen = now;
	if (it->second.blockedUntil <= now) {
		it->second.blockedUntil = {};
		return true;
	}
	const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
		it->second.blockedUntil - now).count();
	retryAfterSeconds = static_cast<int>(std::max<int64_t>(1, remaining + 1));
	return false;
}

bool CKernel::recordLoginFailure(const std::string& accountKey)
{
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(m_loginAttemptsMutex);
	if (m_loginAttempts.find(accountKey) == m_loginAttempts.end() &&
		m_loginAttempts.size() >= kMaxLoginAttemptEntries) {
		auto oldest = m_loginAttempts.begin();
		for (auto it = m_loginAttempts.begin(); it != m_loginAttempts.end(); ++it) {
			if (it->second.lastSeen < oldest->second.lastSeen) oldest = it;
		}
		m_loginAttempts.erase(oldest);
	}
	LoginAttempt& attempt = m_loginAttempts[accountKey];
	if (attempt.lastSeen != std::chrono::steady_clock::time_point{} &&
		now - attempt.lastSeen > kLoginFailureWindow) {
		attempt.failures = 0;
	}
	attempt.lastSeen = now;
	++attempt.failures;
	if (attempt.failures < kMaxLoginFailures) return false;
	attempt.failures = 0;
	attempt.blockedUntil = now + kLoginBlockDuration;
	return true;
}

void CKernel::clearLoginFailures(const std::string& accountKey)
{
	std::lock_guard<std::mutex> lock(m_loginAttemptsMutex);
	m_loginAttempts.erase(accountKey);
}

//打开服务器（打开网络 连接数据库）
bool CKernel::startServer() {
	// 先建立数据库连接池，再开放监听端口，避免启动窗口期收到无法处理的请求。
	const char* ip = std::getenv("IM_DB_HOST");
	const char* user = std::getenv("IM_DB_USER");
	const char* pass = std::getenv("IM_DB_PASSWORD");
	const char* db = std::getenv("IM_DB_NAME");
	std::size_t poolSize = 4;
	if (const char* configuredPoolSize = std::getenv("IM_DB_POOL_SIZE")) {
		char* end = nullptr;
		const unsigned long value = std::strtoul(configuredPoolSize, &end, 10);
		if (end != configuredPoolSize && *end == '\0' && value > 0 && value <= 64) {
			poolSize = static_cast<std::size_t>(value);
		}
	}
	if (!m_sql.ConnectMySql(
		ip ? ip : "127.0.0.1",
		user ? user : "imserver",
		pass ? pass : "",
		db ? db : "imdb",
		3306,
		poolSize)) {
		Logger::error("server_database_start_failed");
		return false;
	}

	m_pMediator = new TcpServerMediator;
	if (!m_pMediator->openNet()) {
		Logger::error("server_network_start_failed");
		delete m_pMediator;
		m_pMediator = nullptr;
		m_sql.DisConnect();
		return false;
	}

	return true;
}
//关闭服务器（回收资源 关闭网络 断开数据库）
void CKernel::endSrever() {
	if (m_pMediator) {
		m_pMediator->closeNet();
		delete m_pMediator;
		m_pMediator = nullptr;
	}
	m_sql.DisConnect();
}

// 处理网络层接收到的所有数据包
void CKernel::dealData(char* data, int len, uintptr_t from)
{
	// data 是网络层动态申请的数据包内存。
	// 如果 data 为空，说明没有有效的数据可以处理。
	if (data == nullptr) {
		Logger::warning("invalid_packet", {
			{"connection_id", std::to_string(from)},
			{"reason", "null_data"}
		});
		return;
	}

	// 每个协议结构体的第一个成员都是 packtype 类型的协议号。
	// 因此，读取协议号前必须确保数据包长度至少能容纳一个 packtype。
	// 如果长度不足仍然读取，会访问数据包以外的内存，造成越界访问。
	if (len < static_cast<int>(sizeof(packtype))) {
		Logger::warning("invalid_packet", {
			{"connection_id", std::to_string(from)},
			{"length", std::to_string(len)},
			{"reason", "packet_too_short"}
		});

		// data 是网络层使用 new[] 申请的。
		// 提前返回前需要释放，避免内存泄漏。
		delete[] data;
		return;
	}

	// 从数据包开头复制出协议号。
	// 使用 memcpy，避免直接把 char* 转换成 int*
	// 可能带来的内存未对齐和非法访问问题。
	packtype type = 0;
	memcpy(&type, data, sizeof(type));

	// 协议号从 DEF_PROTOCOL_BASE + 1 开始。
	// 将协议号转换为协议处理函数数组的下标。
	int index = type - DEF_PROTOCOL_BASE - 1;

	// 下标必须同时满足：
	// 1. 大于等于 0；
	// 2. 小于协议处理函数数组长度。
	// 这里必须使用 &&，不能使用 ||。
	if (index >= 0 && index < DEF_PROTOCOL_COUNT) {
		// 根据下标取得对应的成员函数指针。
		PFUN pf = m_protocol[index];

		// 判断当前协议是否已经注册了处理函数。
		if (pf != nullptr) {
			// 调用具体的协议处理函数。
			(this->*pf)(data, len, from);
		}
		else {
			// 协议号在合法范围内，但没有设置对应的处理函数。
			Logger::warning("unsupported_protocol", {
				{"connection_id", std::to_string(from)},
				{"protocol_type", std::to_string(type)}
			});
		}
	}
	else {
		// 协议号无法转换为合法的数组下标。
		// 可能是数据包损坏、双方协议不一致或者非法数据。
		Logger::warning("invalid_protocol", {
			{"connection_id", std::to_string(from)},
			{"protocol_type", std::to_string(type)}
		});
	}

	// 数据包处理结束，统一释放网络层申请的内存。
	// 各个具体协议处理函数中不要再次 delete[] data。
	delete[] data;
}
//处理注册请求
void CKernel::dealRegisterRq(char* data, int len, uintptr_t from) {

		// 注册请求必须是一个完整的 STRU_REGISTER_RQ。
		// 长度不一致说明数据包不完整、协议不匹配或包含非法数据。
		if (len != static_cast<int>(sizeof(STRU_REGISTER_RQ))) {
			Logger::warning("register_request_rejected", {
				{"connection_id", std::to_string(from)},
				{"length", std::to_string(len)},
				{"reason", "invalid_length"}
			});
			return;
	}
	
	// 长度校验通过后，才可以把数据转换为注册请求结构体。
	//1拆包
	STRU_REGISTER_RQ* rq =reinterpret_cast<STRU_REGISTER_RQ*>(data);
	// 网络结构体中的字符数组必须包含字符串结束符，避免越界读取。
	if (strnlen_s(rq->tel, sizeof(rq->tel)) == sizeof(rq->tel) ||
		strnlen_s(rq->name, sizeof(rq->name)) == sizeof(rq->name) ||
		strnlen_s(rq->password, sizeof(rq->password)) == sizeof(rq->password)) {
		Logger::warning("register_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "unterminated_string"}
		});
		return;
	}
	// 无论后续在哪个分支返回，都在离开处理函数时擦除明文密码缓冲区。
	[[maybe_unused]] SensitiveBufferCleaner passwordCleaner(
		rq->password, sizeof(rq->password));

	std::string hashedPassword;
	if (!passwordForStorage(rq->password, hashedPassword)) {
		Logger::error("password_hash_failed", {{"operation", "register"}});
		return;
	}
	// 对来自客户端的字符串进行 SQL 转义。
	std::string escapedTel;
	std::string escapedName;
	std::string escapedPassword;

	if (!m_sql.EscapeString(rq->tel, escapedTel) ||
		!m_sql.EscapeString(rq->name, escapedName) ||
		!m_sql.EscapeString(hashedPassword.c_str(), escapedPassword)) {

		Logger::error("register_data_escape_failed", {
			{"connection_id", std::to_string(from)}
		});
		return;
	}
	//2校验电话号码是否未注册
	//数据库 根据电话号码查询
	list<string> lstStr;
	char sql[1024] = "";
	sprintf_s(
		sql,
		"select tel from t_user where tel ='%s';",
		escapedTel.c_str()
	);
	if (!m_sql.SelectMySql(sql/*要执行的sql语句*/,
							1/*sql语句中查询列的个数*/,
							lstStr/*sql语句查询到的结果*/)) {

		//1没有链接数据库 sql有语法错误 或者列明和表对应不上（日志中打印的sql语句拷贝到workbanch里面运行）
		Logger::error("register_database_query_failed", {{"stage", "telephone"}});
		return;
	}
	//判断查询结果是否为空
	if (0 != lstStr.size()) {
		//说明电话号码被注册过 注册失败 def_register_telexists
		STRU_REGISTER_RS rs;
		rs.result = def_register_telexists;
		//给客户端回复注册结果
		m_pMediator->sendData((char*)&rs, sizeof(rs), from);
		//结束
		return;
	}
	//3校验昵称是否未注册
	//根据昵称查询
	sprintf_s(
		sql,
		"select name from t_user where name ='%s';",
		escapedName.c_str()
	);
	if (!m_sql.SelectMySql(sql/*要执行的sql语句*/,
							1/*sql语句中查询列的个数*/,
							lstStr/*sql语句查询到的结果*/)) {

		//1没有链接数据库 sql有语法错误 或者列明和表对应不上（日志中打印的sql语句拷贝到workbanch里面运行）
		Logger::error("register_database_query_failed", {{"stage", "name"}});
		return;
	}
	//判断查询结果是否为空
	if (0 != lstStr.size()) {
		//说明电话号码被注册过 注册失败 def_register_nameexists
		STRU_REGISTER_RS rs;
		rs.result = def_register_nameexists;
		//给客户端回复注册结果
		m_pMediator->sendData((char*)&rs, sizeof(rs), from);
		//结束
		return;
	}

	//4数据插入数据库
	sprintf_s(
		sql,
		"insert into t_user"
		"(name,tel,password,felling,iconid)"
		" values('%s','%s','%s','默认签名',8);",
		escapedName.c_str(),
		escapedTel.c_str(),
		escapedPassword.c_str()
	);
	if (!m_sql.UpdateMySql(sql)) {
		//1没有链接数据库 sql有语法错误 或者列明和表对应不上（日志中打印的sql语句拷贝到workbanch里面运行）
		Logger::error("register_database_insert_failed");
			return;
	}

	//5注册成功
	STRU_REGISTER_RS rs;
	rs.result = def_register_success;
	//6给客户端回复注册结果
	m_pMediator->sendData((char*)&rs, sizeof(rs), from);
	Logger::info("user_registered", {{"connection_id", std::to_string(from)}});
}
//处理登录请求
void CKernel::dealLoginRq(char* data, int len, uintptr_t from) {
	// 登录请求必须包含完整的手机号和密码字段。
	if (len != static_cast<int>(sizeof(STRU_LOGIN_RQ))) {
		Logger::warning("login_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"length", std::to_string(len)},
			{"reason", "invalid_length"}
		});
		return;
	}
	//1拆包
	STRU_LOGIN_RQ* rq =reinterpret_cast<STRU_LOGIN_RQ*>(data);
	// 网络结构体中的字符数组必须包含字符串结束符，避免越界读取。
	if (strnlen_s(rq->tel, sizeof(rq->tel)) == sizeof(rq->tel) ||
		strnlen_s(rq->password, sizeof(rq->password)) == sizeof(rq->password)) {
		Logger::warning("login_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "unterminated_string"}
		});
		return;
		
	}
	// 登录完成后立即擦除网络包中的明文密码，减少进程内存残留时间。
	[[maybe_unused]] SensitiveBufferCleaner passwordCleaner(
		rq->password, sizeof(rq->password));

	// 限流键只保存手机号摘要，不在内存和日志中保留原始手机号。
	std::string accountKey;
	if (!sha256Hex(rq->tel, accountKey)) {
		return;
	}
	STRU_LOGIN_RS rs;
	int retryAfterSeconds = 0;
	if (!isLoginAllowed(accountKey, retryAfterSeconds)) {
		rs.result = def_login_password_error;
		m_pMediator->sendData((char*)&rs, sizeof(rs), from);
		++ServerMetrics::instance().rateLimitedLogins;
		Logger::warning("login_rate_limited", {
			{"connection_id", std::to_string(from)},
			{"retry_after_seconds", std::to_string(retryAfterSeconds)}
		});
		return;
	}
	//2根据电话号码查密码 
	// 对客户端传入的电话号码进行 SQL 转义，避免特殊字符破坏查询语句。
	std::string escapedTel;
	if (!m_sql.EscapeString(rq->tel, escapedTel)) {
		Logger::error("login_data_escape_failed", {
			{"connection_id", std::to_string(from)}
		});
		return;
	}

	// 根据转义后的电话号码查询用户。
	list<string> lstStr;
	char sql[1024] = "";
	sprintf_s(
		sql,
		"select password,id from t_user where tel ='%s';",
		escapedTel.c_str()
	);
	if (!m_sql.SelectMySql(sql/*要执行的sql语句*/,
		2/*sql语句中查询列的个数*/,
		lstStr/*sql语句查询到的结果*/)) {

		//1没有链接数据库 sql有语法错误 或者列明和表对应不上（日志中打印的sql语句拷贝到workbanch里面运行）
		Logger::error("login_database_query_failed");
		return;
	}
	//3判断查询结果是否为空
	if (0 == lstStr.size()) {
		//空说明电话号码未注册 登陆失败
		rs.result = def_login_telnotexists;
	}
	else {
		//3比较查询到密码与输入密码
		string pass = lstStr.front();
		lstStr.pop_front();
		int userId = stoi(lstStr.front());
		lstStr.pop_front();
		bool passwordNeedsUpgrade = false;
		if (verifyPassword(rq->password, pass, passwordNeedsUpgrade)) {
			//相等 登录成功
			rs.result = def_login_success;
			rs.userId = userId;
			clearLoginFailures(accountKey);

			// 旧版SHA-256账号在成功登录后透明升级，不影响现有客户端和用户。
			if (passwordNeedsUpgrade) {
				std::string upgradedPassword;
				std::string escapedPassword;
				if (passwordForStorage(rq->password, upgradedPassword) &&
					m_sql.EscapeString(upgradedPassword.c_str(), escapedPassword)) {
					std::ostringstream upgradeSql;
					upgradeSql << "update t_user set password='" << escapedPassword
						<< "' where id=" << userId << ';';
					if (m_sql.UpdateMySql(upgradeSql.str().c_str())) {
						Logger::info("password_hash_upgraded", {
							{"user_id", std::to_string(userId)}
						});
					} else {
						Logger::error("password_hash_upgrade_failed", {
							{"user_id", std::to_string(userId)}
						});
					}
				} else {
					Logger::error("password_hash_upgrade_failed", {
						{"user_id", std::to_string(userId)}
					});
				}
			}
			//从数据库查询好友id
			// 遍历好友id列表
			//判断好友是否在线 在线通知好友自己上线了
			

			//通知在线
			//保存当前用户的socket
			{
				// 同时维护双向映射；重复登录会立即使旧连接失去认证身份。
				std::lock_guard<std::mutex> lock(m_userSocketMutex);
				auto previousConnection = m_mapIdSocket.find(userId);
				if (previousConnection != m_mapIdSocket.end()) {
					m_mapSocketId.erase(previousConnection->second);
				}
				auto previousUser = m_mapSocketId.find(from);
				if (previousUser != m_mapSocketId.end()) {
					m_mapIdSocket.erase(previousUser->second);
				}
				m_mapIdSocket[userId] = from;
				m_mapSocketId[from] = userId;
			}
			//5结果发给客户端
			m_pMediator->sendData((char*)&rs, sizeof(rs), from);
			//获取当前登录用户的信息 以及好友的信息
			getUserInfoAndFriendInfo(userId);
			// 好友信息先进入发送队列，再投递待处理申请、处理结果和离线消息。
			deliverPendingFriendRequests(userId, from);
			deliverPendingFriendResults(userId, from);
			deliverPendingMessages(userId, from);
			Logger::info("user_logged_in", {
				{"connection_id", std::to_string(from)},
				{"user_id", std::to_string(userId)}
			});
			return;

		}
		else {
			//不相等 登陆失败 密码错我
			rs.result = def_login_password_error;
		}
	}

	if (recordLoginFailure(accountKey)) {
		Logger::warning("login_account_temporarily_blocked", {
			{"block_seconds", std::to_string(kLoginBlockDuration.count())}
		});
	}
	//5结果发给客户端
	m_pMediator->sendData((char*)&rs, sizeof(rs), from);
}
//函数指针数组 初始化 存数据
void CKernel::setProtocol() {
	//初始化 0
	memset(m_protocol,0,sizeof(m_protocol));
	//存入数据
	m_protocol[DEF_REGISTER_RQ- DEF_PROTOCOL_BASE-1] = &CKernel::dealRegisterRq;
	m_protocol[DEF_LOGIN_RQ - DEF_PROTOCOL_BASE - 1] = &CKernel::dealLoginRq;
	m_protocol[DEF_CHAT_RQ - DEF_PROTOCOL_BASE - 1] = &CKernel::dealChatRq;
	m_protocol[DEF_OFFLINE_RQ - DEF_PROTOCOL_BASE - 1] = &CKernel::dealOfflineRq;
	m_protocol[DEF_ADD_FRIEND_RQ - DEF_PROTOCOL_BASE - 1] = &CKernel::dealAddFriendRq;
	m_protocol[DEF_ADD_FRIEND_RS - DEF_PROTOCOL_BASE - 1] = &CKernel::dealAddFriendRs;
	m_protocol[DEF_HEARTBEAT_RQ - DEF_PROTOCOL_BASE - 1] = &CKernel::dealHeartbeatRq;
	m_protocol[DEF_MESSAGE_ACK - DEF_PROTOCOL_BASE - 1] = &CKernel::dealMessageAck;
	m_protocol[DEF_HISTORY_RQ - DEF_PROTOCOL_BASE - 1] = &CKernel::dealHistoryRq;
	m_protocol[DEF_FRIEND_RESULT_ACK - DEF_PROTOCOL_BASE - 1] = &CKernel::dealFriendResultAck;
	Logger::info("protocol_handlers_registered", {{"count", "10"}});
}

// 处理客户端心跳。网络层会在收到任意数据时刷新连接活跃时间，
// 此处回复心跳用于让客户端确认双向链路都处于可用状态。
void CKernel::dealHeartbeatRq(char* data, int len, uintptr_t from)
{
	(void)data;
	if (len != static_cast<int>(sizeof(STRU_HEARTBEAT_RQ))) {
		Logger::warning("heartbeat_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"length", std::to_string(len)},
			{"reason", "invalid_length"}
		});
		return;
	}

	STRU_HEARTBEAT_RS response;
	m_pMediator->sendData(
		reinterpret_cast<char*>(&response),
		sizeof(response),
		from
	);
}

void CKernel::dealMessageAck(char* data, int len, uintptr_t from)
{
	if (len != static_cast<int>(sizeof(STRU_MESSAGE_ACK))) {
		Logger::warning("message_ack_rejected", {
			{"connection_id", std::to_string(from)},
			{"length", std::to_string(len)},
			{"reason", "invalid_length"}
		});
		return;
	}

	int receiverId = 0;
	if (!getSocketUserId(from, receiverId)) {
		Logger::warning("message_ack_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "unauthenticated_connection"}
		});
		return;
	}

	const STRU_MESSAGE_ACK* ack = reinterpret_cast<const STRU_MESSAGE_ACK*>(data);
	std::ostringstream sql;
	// receiver_id 条件确保用户只能确认发给自己的消息。
	sql << "update t_message set delivered_at=" << static_cast<int64_t>(std::time(nullptr))
		<< " where id=" << ack->messageId
		<< " and receiver_id=" << receiverId
		<< " and delivered_at is null;";
	if (!m_sql.UpdateMySql(sql.str().c_str())) {
		Logger::error("message_delivery_update_failed", {
			{"message_id", std::to_string(ack->messageId)},
			{"receiver_id", std::to_string(receiverId)}
		});
	}
}

void CKernel::deliverPendingMessages(int userId, ConnectionId connectionId)
{
	std::ostringstream sql;
	sql << "select id,sender_id,receiver_id,content,created_at from t_message "
		<< "where receiver_id=" << userId
		<< " and delivered_at is null order by id asc limit 1000;";

	list<string> result;
	if (!m_sql.SelectMySql(sql.str().c_str(), 5, result)) {
		Logger::error("offline_message_query_failed", {
			{"user_id", std::to_string(userId)}
		});
		return;
	}

	while (result.size() >= 5) {
		STRU_CHAT_MESSAGE message;
		message.messageId = std::stoull(result.front()); result.pop_front();
		message.senderId = std::stoi(result.front()); result.pop_front();
		message.receiverId = std::stoi(result.front()); result.pop_front();
		const string content = result.front(); result.pop_front();
		message.createdAt = std::stoll(result.front()); result.pop_front();
		message.source = 1;
		if (strcpy_s(message.content, sizeof(message.content), content.c_str()) != 0) {
			Logger::warning("offline_message_skipped", {
				{"message_id", std::to_string(message.messageId)},
				{"reason", "content_too_long"}
			});
			continue;
		}
		m_pMediator->sendData(reinterpret_cast<char*>(&message), sizeof(message), connectionId);
	}
}

void CKernel::dealHistoryRq(char* data, int len, uintptr_t from)
{
	if (len != static_cast<int>(sizeof(STRU_HISTORY_RQ))) {
		Logger::warning("history_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"length", std::to_string(len)},
			{"reason", "invalid_length"}
		});
		return;
	}

	int userId = 0;
	if (!getSocketUserId(from, userId)) {
		Logger::warning("history_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "unauthenticated_connection"}
		});
		return;
	}

	const STRU_HISTORY_RQ* request = reinterpret_cast<const STRU_HISTORY_RQ*>(data);
	if (request->friendId <= 0) {
		return;
	}
	const int pageSize = std::max(1, std::min(request->limit, 100));

	std::ostringstream sql;
	sql << "select id,sender_id,receiver_id,content,created_at from t_message where ((sender_id="
		<< userId << " and receiver_id=" << request->friendId << ") or (sender_id="
		<< request->friendId << " and receiver_id=" << userId << "))";
	if (request->beforeMessageId != 0) {
		sql << " and id<" << request->beforeMessageId;
	}
	sql << " order by id desc limit " << (pageSize + 1) << ';';

	list<string> result;
	if (!m_sql.SelectMySql(sql.str().c_str(), 5, result)) {
		Logger::error("history_query_failed", {
			{"friend_id", std::to_string(request->friendId)},
			{"user_id", std::to_string(userId)}
		});
		return;
	}

	std::vector<STRU_CHAT_MESSAGE> messages;
	while (result.size() >= 5) {
		STRU_CHAT_MESSAGE message;
		message.messageId = std::stoull(result.front()); result.pop_front();
		message.senderId = std::stoi(result.front()); result.pop_front();
		message.receiverId = std::stoi(result.front()); result.pop_front();
		const string content = result.front(); result.pop_front();
		message.createdAt = std::stoll(result.front()); result.pop_front();
		message.source = 2;
		if (strcpy_s(message.content, sizeof(message.content), content.c_str()) == 0) {
			messages.push_back(message);
		}
	}

	const bool hasMore = messages.size() > static_cast<size_t>(pageSize);
	if (hasMore) {
		messages.resize(static_cast<size_t>(pageSize));
	}
	const uint64_t oldestMessageId = messages.empty() ? 0 : messages.back().messageId;

	// 数据库按倒序查询，发送时反转为从旧到新的显示顺序。
	for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
		m_pMediator->sendData(reinterpret_cast<char*>(&(*it)), sizeof(*it), from);
	}

	STRU_HISTORY_END end;
	end.friendId = request->friendId;
	end.count = static_cast<int>(messages.size());
	end.hasMore = hasMore ? 1 : 0;
	end.oldestMessageId = oldestMessageId;
	m_pMediator->sendData(reinterpret_cast<char*>(&end), sizeof(end), from);
}

//获取当前登录用户的信息 以及好友的信息
void CKernel::getUserInfoAndFriendInfo(int userId) {
	//根据自己的id查询自己的信息
	STRU_FRIEND_INFO userInfo;
	getInfoById(userId, &userInfo);
	//把自己的信息发送给客户端
	ConnectionId userSocket = 0;

	// 通过统一函数读取当前用户 Socket。
	if (getUserSocket(userId, userSocket)) {
		// 读取完成后已经释放 Map 锁，再进行网络发送。
		m_pMediator->sendData(
			reinterpret_cast<char*>(&userInfo),
			sizeof(userInfo),
			static_cast<uintptr_t>(userSocket)
		);
	}
	else {
		Logger::warning("user_info_delivery_skipped", {
			{"reason", "user_offline"},
			{"user_id", std::to_string(userId)}
		});
		return;
	}
	
	//根据自己的id查询好友的id列表
	list<string> lstStr;
	char sql[1024] = "";
	sprintf_s(sql, "select idB from t_friend where idA ='%d';", userId);
	if (!m_sql.SelectMySql(sql/*要执行的sql语句*/,
		1/*sql语句中查询列的个数*/,
		lstStr/*sql语句查询到的结果*/)) {

		//1没有链接数据库 sql有语法错误 或者列明和表对应不上（日志中打印的sql语句拷贝到workbanch里面运行）
		Logger::error("friend_list_query_failed", {
			{"user_id", std::to_string(userId)}
		});
		return;
	}
	//遍历好友的id列表
	STRU_FRIEND_INFO friendInfo;
	while (lstStr.size()>0)
	{
		//取出好友id
		int friendId = stoi(lstStr.front());
		lstStr.pop_front();
		//根据好友id 查询好友信息
		getInfoById(friendId, &friendInfo);
		//把好友的信息发回个客户端
		ConnectionId currentUserSocket = 0;

		if (getUserSocket(userId, currentUserSocket)) {
			m_pMediator->sendData(
				reinterpret_cast<char*>(&friendInfo),
				sizeof(friendInfo),
				static_cast<uintptr_t>(currentUserSocket)
			);
		}
		else {
			Logger::warning("friend_list_delivery_interrupted", {
				{"reason", "user_offline"},
				{"user_id", std::to_string(userId)}
			});
			return;
		}
		//判断好友是否在线 
		ConnectionId friendSocket = 0;

		if (getUserSocket(friendId, friendSocket)) {
			m_pMediator->sendData(
				reinterpret_cast<char*>(&userInfo),
				sizeof(userInfo),
				static_cast<uintptr_t>(friendSocket)
			);
		}
	}
}
//根据id查询用户信息(查到的用户信息作为输出参数返回STRU_FRIEND_INFO* info)
void CKernel::getInfoById(int id, STRU_FRIEND_INFO* info) {
	info->id = id;
	ConnectionId userSocket = 0;

	// 通过统一函数判断用户是否在线。
	if (getUserSocket(id, userSocket)) {
		info->status = def_status_online;
	}
	else {
		info->status = def_status_offline;
	}
	//从数据库查询 昵称 签名 头像 
	list<string> lstStr;
	char sql[1024] = "";
	sprintf_s(sql, "select name,felling,iconid  from t_user where id ='%d';", id);
	if (!m_sql.SelectMySql(sql/*要执行的sql语句*/,
		3/*sql语句中查询列的个数*/,
		lstStr/*sql语句查询到的结果*/)) {

		//1没有链接数据库 sql有语法错误 或者列明和表对应不上（日志中打印的sql语句拷贝到workbanch里面运行）
		Logger::error("user_profile_query_failed", {
			{"user_id", std::to_string(id)}
		});
		return;
	}
	if (lstStr.size() == 3) {
		//取出昵称
		strcpy_s(info->name,sizeof(info->name),lstStr.front().c_str());
		lstStr.pop_front();
		//取出签名
		strcpy_s(info->feeling, sizeof(info->feeling), lstStr.front().c_str());
		lstStr.pop_front();
		//取出头像
		info->iconId = stoi(lstStr.front());
		lstStr.pop_front();
	}
	else {
		Logger::error("user_profile_result_invalid", {
			{"column_count", std::to_string(lstStr.size())},
			{"user_id", std::to_string(id)}
		});
	}
}
void  CKernel::dealChatRq(char* data, int len, uintptr_t from) {
	if (len != static_cast<int>(sizeof(STRU_CHAT_RQ))) {
		Logger::warning("chat_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"length", std::to_string(len)},
			{"reason", "invalid_length"}
		});
		return;
	}
	STRU_CHAT_RQ* rq =reinterpret_cast<STRU_CHAT_RQ*>(data);
	if (strnlen_s(rq->content, sizeof(rq->content)) ==
		sizeof(rq->content)) {
		Logger::warning("chat_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "unterminated_content"}
		});
		return;
	}
	if (rq->userId <= 0 || rq->friendId <= 0) {
		Logger::warning("chat_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "invalid_user_id"}
		});
		return;
	}

	// 请求中的发送者必须与当前登录连接一致，禁止伪造用户身份。
	ConnectionId senderSocket = 0;
	if (!getUserSocket(rq->userId, senderSocket) ||
		senderSocket != from) {
		Logger::warning("chat_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "identity_mismatch"},
			{"user_id", std::to_string(rq->userId)}
		});
		return;
	}

	// 服务端再次校验好友关系，不能只信任客户端传入的接收者 ID。
	std::ostringstream friendSql;
	friendSql << "select 1 from t_friend where idA=" << rq->userId
		<< " and idB=" << rq->friendId << " limit 1;";
	list<string> friendResult;
	if (!m_sql.SelectMySql(friendSql.str().c_str(), 1, friendResult) ||
		friendResult.empty()) {
		Logger::warning("chat_request_rejected", {
			{"friend_id", std::to_string(rq->friendId)},
			{"reason", "not_friends"},
			{"user_id", std::to_string(rq->userId)}
		});
		STRU_CHAT_RS response;
		response.friendId = rq->friendId;
		m_pMediator->sendData(reinterpret_cast<char*>(&response), sizeof(response), from);
		return;
	}

	std::string escapedContent;
	if (!m_sql.EscapeString(rq->content, escapedContent)) {
		Logger::error("chat_content_escape_failed", {
			{"user_id", std::to_string(rq->userId)}
		});
		return;
	}

	// 消息先持久化，再尝试实时投递；即使接收者离线也不会丢失。
	const int64_t createdAt = static_cast<int64_t>(std::time(nullptr));
	std::ostringstream insertSql;
	insertSql << "insert into t_message(sender_id,receiver_id,content,created_at) values("
		<< rq->userId << ',' << rq->friendId << ",'" << escapedContent << "',"
		<< createdAt << ");";
	uint64_t messageId = 0;
	if (!m_sql.InsertMySql(insertSql.str().c_str(), messageId)) {
		Logger::error("chat_message_persist_failed", {
			{"friend_id", std::to_string(rq->friendId)},
			{"user_id", std::to_string(rq->userId)}
		});
		STRU_CHAT_RS response;
		response.friendId = rq->friendId;
		m_pMediator->sendData(reinterpret_cast<char*>(&response), sizeof(response), from);
		return;
	}

	STRU_CHAT_MESSAGE message;
	message.messageId = messageId;
	message.senderId = rq->userId;
	message.receiverId = rq->friendId;
	message.createdAt = createdAt;
	message.source = 0;
	strcpy_s(message.content, sizeof(message.content), rq->content);

	// 在线时立即发送；离线时保留 delivered_at 为空，待下次登录补发。
	ConnectionId friendSocket = 0;
	if (getUserSocket(rq->friendId, friendSocket)) {
		m_pMediator->sendData(
			reinterpret_cast<char*>(&message),
			sizeof(message),
			static_cast<uintptr_t>(friendSocket)
		);
	}

	// 保存成功即表示服务端已经可靠接收，接收者离线不再视为发送失败。
	STRU_CHAT_RS response;
	response.friendId = rq->friendId;
	response.result = def_send_success;
	m_pMediator->sendData(reinterpret_cast<char*>(&response), sizeof(response), from);
}
//处理下线请求
// 处理客户端主动下线请求。
void CKernel::dealOfflineRq(
	char* data,
	int len,
	uintptr_t from)
{
	// 校验下线请求结构体长度。
	if (len != static_cast<int>(sizeof(STRU_OFFLINE_RQ))) {
		Logger::warning("offline_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"length", std::to_string(len)},
			{"reason", "invalid_length"}
		});
		return;
	}

	// 校验数据包格式，但不直接信任请求中的 userId。
	// 用户真实身份应该由服务端维护的 Socket 映射决定。
	STRU_OFFLINE_RQ* rq =
		reinterpret_cast<STRU_OFFLINE_RQ*>(data);

	// 当前 rq->userId 仅用于兼容旧协议，
	// 实际下线用户由 from 对应的 Socket 决定。
	(void)rq;

	// 复用统一的断线处理逻辑：
	// 1. 根据 Socket 查找真实 userId；
	// 2. 从在线用户 Map 中删除；
	// 3. 查询好友；
	// 4. 通知在线好友下线。
	dealDisconnect(from);
}
//处理添加好友请求
void CKernel::dealAddFriendRq(char* data, int len, uintptr_t from) {
	if (len != static_cast<int>(sizeof(STRU_ADD_FRIEND_RQ))) {
		Logger::warning("friend_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"length", std::to_string(len)},
			{"reason", "invalid_length"}
		});
		return;
	}
	STRU_ADD_FRIEND_RQ* rq =reinterpret_cast<STRU_ADD_FRIEND_RQ*>(data);
	if (strnlen_s(rq->userName, sizeof(rq->userName)) ==
		sizeof(rq->userName) ||
		strnlen_s(rq->friendName, sizeof(rq->friendName)) ==
		sizeof(rq->friendName)) {
		Logger::warning("friend_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "unterminated_name"}
		});
		return;
	}
	if (rq->userId <= 0) {
		Logger::warning("friend_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "invalid_user_id"}
		});
		return;
	}

	// 请求者必须与当前登录连接一致，防止冒用其他用户身份。
	ConnectionId senderSocket = 0;
	if (!getUserSocket(rq->userId, senderSocket) ||
		senderSocket != from) {
		Logger::warning("friend_request_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "identity_mismatch"},
			{"user_id", std::to_string(rq->userId)}
		});
		return;
	}

	std::string escapedFriendName;
	if (!m_sql.EscapeString(rq->friendName, escapedFriendName)) {
		Logger::error("friend_name_escape_failed", {
			{"user_id", std::to_string(rq->userId)}
		});
		return;
	}

	// 根据昵称查询目标用户，目标不在线时仍可继续保存申请。
	list<string> lstStr;
	char sql[1024] = "";
	sprintf_s(sql, "select id from t_user where name='%s';", escapedFriendName.c_str());
	if (!m_sql.SelectMySql(sql, 1, lstStr)) {
		Logger::error("friend_target_query_failed", {
			{"user_id", std::to_string(rq->userId)}
		});
		return;
	}
	if (lstStr.empty()) {
		STRU_ADD_FRIEND_RS rs;
		rs.result = def_add_friend_notexists;
		strcpy_s(rs.friendName, sizeof(rs.friendName), rq->friendName);
		m_pMediator->sendData(reinterpret_cast<char*>(&rs), sizeof(rs), from);
		return;
	}

	const int friendId = stoi(lstStr.front());
	if (friendId == rq->userId) {
		Logger::warning("friend_request_rejected", {
			{"reason", "self_request"},
			{"user_id", std::to_string(rq->userId)}
		});
		return;
	}

	// 已经是好友时直接返回成功，保证重复请求具有幂等性。
	std::ostringstream relationshipSql;
	relationshipSql << "select 1 from t_friend where idA=" << rq->userId
		<< " and idB=" << friendId << " limit 1;";
	list<string> relationshipResult;
	if (!m_sql.SelectMySql(relationshipSql.str().c_str(), 1, relationshipResult)) {
		Logger::error("friend_relationship_query_failed", {
			{"friend_id", std::to_string(friendId)},
			{"user_id", std::to_string(rq->userId)}
		});
		return;
	}
	if (!relationshipResult.empty()) {
		STRU_ADD_FRIEND_RS rs;
		rs.result = def_add_friend_success;
		rs.userId = rq->userId;
		rs.friendId = friendId;
		strcpy_s(rs.userName, sizeof(rs.userName), rq->userName);
		strcpy_s(rs.friendName, sizeof(rs.friendName), rq->friendName);
		m_pMediator->sendData(reinterpret_cast<char*>(&rs), sizeof(rs), from);
		return;
	}

	// 使用唯一键覆盖之前已拒绝的申请，使再次申请仍可进入待处理状态。
	const int64_t createdAt = static_cast<int64_t>(std::time(nullptr));
	std::ostringstream saveSql;
	saveSql << "insert into t_friend_request(requester_id,target_id,status,created_at,handled_at) values("
		<< rq->userId << ',' << friendId << ",0," << createdAt << ",null) "
		<< "on duplicate key update status=0,created_at=" << createdAt
		<< ",handled_at=null,result_delivered_at=null;";
	if (!m_sql.UpdateMySql(saveSql.str().c_str())) {
		Logger::error("friend_request_persist_failed", {
			{"friend_id", std::to_string(friendId)},
			{"user_id", std::to_string(rq->userId)}
		});
		return;
	}

	// 目标在线则立即投递；离线则在其下次登录时从数据库补发。
	ConnectionId friendSocket = 0;
	if (getUserSocket(friendId, friendSocket)) {
		m_pMediator->sendData(data, len, static_cast<uintptr_t>(friendSocket));
	}

	STRU_ADD_FRIEND_RS pending;
	pending.result = def_add_friend_pending;
	pending.userId = rq->userId;
	pending.friendId = friendId;
	strcpy_s(pending.userName, sizeof(pending.userName), rq->userName);
	strcpy_s(pending.friendName, sizeof(pending.friendName), rq->friendName);
	m_pMediator->sendData(reinterpret_cast<char*>(&pending), sizeof(pending), from);
	Logger::info("friend_request_saved", {
		{"friend_id", std::to_string(friendId)},
		{"user_id", std::to_string(rq->userId)}
	});
}

void CKernel::deliverPendingFriendRequests(int userId, ConnectionId connectionId)
{
	std::ostringstream sql;
	sql << "select r.requester_id,a.name,b.name from t_friend_request r "
		<< "join t_user a on a.id=r.requester_id join t_user b on b.id=r.target_id "
		<< "where r.target_id=" << userId << " and r.status=0 order by r.id asc;";
	list<string> result;
	if (!m_sql.SelectMySql(sql.str().c_str(), 3, result)) {
		Logger::error("pending_friend_request_query_failed", {
			{"user_id", std::to_string(userId)}
		});
		return;
	}
	while (result.size() >= 3) {
		STRU_ADD_FRIEND_RQ request;
		request.userId = stoi(result.front()); result.pop_front();
		const string requesterName = result.front(); result.pop_front();
		const string targetName = result.front(); result.pop_front();
		if (strcpy_s(request.userName, sizeof(request.userName), requesterName.c_str()) != 0 ||
			strcpy_s(request.friendName, sizeof(request.friendName), targetName.c_str()) != 0) {
			Logger::warning("pending_friend_request_skipped", {
				{"reason", "name_too_long"},
				{"user_id", std::to_string(userId)}
			});
			continue;
		}
		m_pMediator->sendData(reinterpret_cast<char*>(&request), sizeof(request), connectionId);
	}
}

void CKernel::deliverPendingFriendResults(int userId, ConnectionId connectionId)
{
	std::ostringstream sql;
	sql << "select r.id,r.status,r.target_id,coalesce(r.handled_at,0),a.name,b.name "
		<< "from t_friend_request r "
		<< "join t_user a on a.id=r.requester_id "
		<< "join t_user b on b.id=r.target_id "
		<< "where r.requester_id=" << userId
		<< " and r.status in(1,2) and r.result_delivered_at is null "
		<< "order by r.id asc;";
	list<string> result;
	if (!m_sql.SelectMySql(sql.str().c_str(), 6, result)) {
		Logger::error("pending_friend_result_query_failed", {
			{"user_id", std::to_string(userId)}
		});
		return;
	}

	while (result.size() >= 6) {
		STRU_FRIEND_RESULT_NOTIFY notification;
		notification.requestId = std::stoull(result.front()); result.pop_front();
		const int status = std::stoi(result.front()); result.pop_front();
		notification.result = status == 1
			? def_add_friend_success : def_add_friend_refuse;
		notification.requesterId = userId;
		notification.targetId = std::stoi(result.front()); result.pop_front();
		notification.handledAt = std::stoll(result.front()); result.pop_front();
		const string requesterName = result.front(); result.pop_front();
		const string targetName = result.front(); result.pop_front();
		if (strcpy_s(notification.requesterName, sizeof(notification.requesterName),
			requesterName.c_str()) != 0 ||
			strcpy_s(notification.targetName, sizeof(notification.targetName),
			targetName.c_str()) != 0) {
			Logger::warning("pending_friend_result_skipped", {
				{"reason", "name_too_long"},
				{"request_id", std::to_string(notification.requestId)}
			});
			continue;
		}
		m_pMediator->sendData(
			reinterpret_cast<char*>(&notification),
			sizeof(notification),
			connectionId);
	}
}

void CKernel::dealFriendResultAck(char* data, int len, uintptr_t from)
{
	if (len != static_cast<int>(sizeof(STRU_FRIEND_RESULT_ACK))) {
		Logger::warning("friend_result_ack_rejected", {
			{"connection_id", std::to_string(from)},
			{"length", std::to_string(len)},
			{"reason", "invalid_length"}
		});
		return;
	}

	int requesterId = 0;
	if (!getSocketUserId(from, requesterId)) {
		Logger::warning("friend_result_ack_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "unauthenticated_connection"}
		});
		return;
	}

	const STRU_FRIEND_RESULT_ACK* ack =
		reinterpret_cast<const STRU_FRIEND_RESULT_ACK*>(data);
	if (ack->requestId == 0) {
		Logger::warning("friend_result_ack_rejected", {
			{"reason", "invalid_request_id"},
			{"user_id", std::to_string(requesterId)}
		});
		return;
	}

	std::ostringstream sql;
	// requester_id条件确保用户只能确认属于自己的处理结果。
	sql << "update t_friend_request set result_delivered_at="
		<< static_cast<int64_t>(std::time(nullptr))
		<< " where id=" << ack->requestId
		<< " and requester_id=" << requesterId
		<< " and status in(1,2) and result_delivered_at is null;";
	if (!m_sql.UpdateMySql(sql.str().c_str())) {
		Logger::error("friend_result_delivery_update_failed", {
			{"request_id", std::to_string(ack->requestId)},
			{"user_id", std::to_string(requesterId)}
		});
	}
}
//处理添加好友回复
void CKernel::dealAddFriendRs(char* data, int len, uintptr_t from) {
	// 添加好友回复必须是完整的 STRU_ADD_FRIEND_RS。
	if (len != static_cast<int>(sizeof(STRU_ADD_FRIEND_RS))) {
		Logger::warning("friend_response_rejected", {
			{"connection_id", std::to_string(from)},
			{"length", std::to_string(len)},
			{"reason", "invalid_length"}
		});
		return;
	}
	//1拆包
	STRU_ADD_FRIEND_RS* rs =reinterpret_cast<STRU_ADD_FRIEND_RS*>(data);
	// 响应中的 ID 必须是有效用户 ID。
	if (rs->userId <= 0 || rs->friendId <= 0) {
		Logger::warning("friend_response_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "invalid_user_id"}
		});
		return;
	}

	// 响应中的昵称字段必须以字符串结束符结尾。
	if (strnlen_s(rs->userName, sizeof(rs->userName)) ==
		sizeof(rs->userName) ||
		strnlen_s(rs->friendName, sizeof(rs->friendName)) ==
		sizeof(rs->friendName)) {
		Logger::warning("friend_response_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "unterminated_name"}
		});
		return;
	}

	// 只有被添加好友本人对应的连接，才能发送这个响应。
	ConnectionId senderSocket = 0;
	if (!getUserSocket(rs->friendId, senderSocket) ||
		senderSocket != from) {
		Logger::warning("friend_response_rejected", {
			{"connection_id", std::to_string(from)},
			{"reason", "identity_mismatch"},
			{"user_id", std::to_string(rs->friendId)}
		});
		return;
	}
	// 被申请人只能选择同意或拒绝，其他结果不能用于处理数据库申请。
	if (rs->result != def_add_friend_success &&
		rs->result != def_add_friend_refuse) {
		Logger::warning("friend_response_rejected", {
			{"reason", "invalid_result"},
			{"user_id", std::to_string(rs->friendId)}
		});
		return;
	}

	// 必须存在对应的待处理记录，防止客户端伪造好友申请响应。
	std::ostringstream pendingSql;
	pendingSql << "select r.id,a.name,b.name from t_friend_request r "
		<< "join t_user a on a.id=r.requester_id "
		<< "join t_user b on b.id=r.target_id "
		<< "where r.requester_id=" << rs->userId
		<< " and r.target_id=" << rs->friendId << " and r.status=0 limit 1;";
	list<string> pendingResult;
	if (!m_sql.SelectMySql(pendingSql.str().c_str(), 3, pendingResult) ||
		pendingResult.size() != 3) {
		Logger::warning("friend_response_rejected", {
			{"friend_id", std::to_string(rs->userId)},
			{"reason", "pending_request_not_found"},
			{"user_id", std::to_string(rs->friendId)}
		});
		return;
	}
	const uint64_t requestId = std::stoull(pendingResult.front()); pendingResult.pop_front();
	const string requesterName = pendingResult.front(); pendingResult.pop_front();
	const string targetName = pendingResult.front(); pendingResult.pop_front();

	const int64_t handledAt = static_cast<int64_t>(std::time(nullptr));
	if (def_add_friend_success == rs->result) {
		// 双向关系写入和申请状态更新放在同一个事务中。
		char firstSql[1024] = "";
		char secondSql[1024] = "";

		sprintf_s(
			firstSql,
			"insert ignore into t_friend(idA,idB) values('%d','%d'),('%d','%d');",
			rs->friendId, rs->userId, rs->userId, rs->friendId
		);

		sprintf_s(
			secondSql,
			"update t_friend_request set status=1,handled_at=%lld,result_delivered_at=null where requester_id=%d and target_id=%d and status=0;",
			static_cast<long long>(handledAt), rs->userId, rs->friendId
		);

		if (!m_sql.UpdateMySqlTransaction(firstSql, secondSql)) {
			Logger::error("friend_relationship_transaction_failed", {
				{"friend_id", std::to_string(rs->userId)},
				{"user_id", std::to_string(rs->friendId)}
			});
			return;
		}

		// 数据库写入成功后，再刷新双方好友列表。
		getUserInfoAndFriendInfo(rs->friendId);
	}
	else {
		std::ostringstream refuseSql;
		refuseSql << "update t_friend_request set status=2,handled_at=" << handledAt
			<< ",result_delivered_at=null"
			<< " where requester_id=" << rs->userId << " and target_id="
			<< rs->friendId << " and status=0;";
		if (!m_sql.UpdateMySql(refuseSql.str().c_str())) {
			Logger::error("friend_rejection_persist_failed", {
				{"friend_id", std::to_string(rs->userId)},
				{"user_id", std::to_string(rs->friendId)}
			});
			return;
		}
	}
	// 构造可靠结果通知；申请者离线时保留未确认状态，登录后再补发。
	STRU_FRIEND_RESULT_NOTIFY notification;
	notification.requestId = requestId;
	notification.result = rs->result;
	notification.requesterId = rs->userId;
	notification.targetId = rs->friendId;
	notification.handledAt = handledAt;
	if (strcpy_s(notification.requesterName, sizeof(notification.requesterName),
		requesterName.c_str()) != 0 ||
		strcpy_s(notification.targetName, sizeof(notification.targetName),
		targetName.c_str()) != 0) {
		Logger::warning("friend_result_notification_skipped", {
			{"reason", "name_too_long"},
			{"request_id", std::to_string(requestId)}
		});
		return;
	}

	// 查找最初发起好友申请的用户 Socket。
	ConnectionId userSocket = 0;

	if (getUserSocket(rs->userId, userSocket)) {
		// 释放Map锁后发送；只有收到客户端ACK才更新送达时间。
		m_pMediator->sendData(
			reinterpret_cast<char*>(&notification),
			sizeof(notification),
			static_cast<uintptr_t>(userSocket)
		);
	}
	Logger::info("friend_request_handled", {
		{"request_id", std::to_string(requestId)},
		{"result", std::to_string(rs->result)},
		{"user_id", std::to_string(rs->friendId)}
	});

}
// 处理客户端连接断开事件。
void CKernel::dealDisconnect(ConnectionId connectionId)
{
	// 使用连接到用户的反向映射进行 O(1) 查找。
	int userId = -1;
	{
		std::lock_guard<std::mutex> lock(m_userSocketMutex);
		auto reverse = m_mapSocketId.find(connectionId);
		if (reverse != m_mapSocketId.end()) {
			userId = reverse->second;
			m_mapSocketId.erase(reverse);
			auto forward = m_mapIdSocket.find(userId);
			// 仅删除仍指向本连接的记录，不能误删较新的重复登录会话。
			if (forward != m_mapIdSocket.end() && forward->second == connectionId) {
				m_mapIdSocket.erase(forward);
			}
		}
	}
	// 没有找到对应用户，说明该连接可能尚未登录，
	// 或者已经被其他流程清理过。
	if (userId == -1) {
		return;
	}

	// 主动下线和随后发生的 Socket 关闭可能重复进入本函数。
	// 只有首次成功移除登录映射时才记录业务断线，避免产生重复日志。
	Logger::info("connection_disconnected", {
		{"connection_id", std::to_string(connectionId)},
		{"user_id", std::to_string(userId)}
	});

	// 查询当前用户的好友列表。
	list<string> friendIdList;
	char sql[1024] = "";

	sprintf_s(
		sql,
		"select idB from t_friend where idA = '%d';",
		userId
	);

	if (!m_sql.SelectMySql(sql, 1, friendIdList)) {
		Logger::error("friend_list_query_failed", {
			{"user_id", std::to_string(userId)}
		});
		return;
	}

	// 构造下线通知。
	STRU_OFFLINE_RQ offlineRequest;
	offlineRequest.userId = userId;

	// 通知所有仍在线的好友。
	while (!friendIdList.empty()) {
		int friendId = stoi(friendIdList.front());
		friendIdList.pop_front();

		ConnectionId friendSocket = 0;

		{
			// 读取好友在线 Socket 时必须加锁。
			std::lock_guard<std::mutex> lock(m_userSocketMutex);

			auto friendIt = m_mapIdSocket.find(friendId);

			if (friendIt != m_mapIdSocket.end()) {
				friendSocket = friendIt->second;
			}
		}

		// 发送操作不要放在锁里面，避免网络阻塞时阻塞其他业务。
		if (friendSocket != 0) {
			m_pMediator->sendData(
				reinterpret_cast<char*>(&offlineRequest),
				sizeof(offlineRequest),
				static_cast<uintptr_t>(friendSocket)
			);
		}
	}

	Logger::info("user_logged_out", {
		{"connection_id", std::to_string(connectionId)},
		{"user_id", std::to_string(userId)}
	});
}
// 查询用户当前在线的唯一连接编号。
bool CKernel::getUserSocket(int userId, ConnectionId& connectionId)
{
	// 读取在线用户 Map 时加锁。
	std::lock_guard<std::mutex> lock(m_userSocketMutex);

	auto it = m_mapIdSocket.find(userId);

	if (it == m_mapIdSocket.end()) {
		// 用户不在线。
		connectionId = 0;
		return false;
	}

	// 返回用户对应的连接编号。
	connectionId = it->second;
	return true;
}

bool CKernel::getSocketUserId(ConnectionId connectionId, int& userId)
{
	std::lock_guard<std::mutex> lock(m_userSocketMutex);
	auto it = m_mapSocketId.find(connectionId);
	if (it != m_mapSocketId.end()) {
		userId = it->second;
		return true;
	}

	userId = 0;
	return false;
}
