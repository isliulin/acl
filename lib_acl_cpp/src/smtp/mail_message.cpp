#include "acl_stdafx.hpp"
#include <list>
#include "acl_cpp/stdlib/log.hpp"
#include "acl_cpp/stdlib/util.hpp"
#include "acl_cpp/stdlib/string.hpp"
#include "acl_cpp/stdlib/dbuf_pool.hpp"
#include "acl_cpp/stream/ofstream.hpp"
#include "acl_cpp/stream/ifstream.hpp"
#include "acl_cpp/stdlib/thread.hpp"
#include "acl_cpp/mime/rfc822.hpp"
#include "acl_cpp/mime/rfc2047.hpp"
#include "acl_cpp/mime/mime_base64.hpp"
#include "acl_cpp/smtp/mail_attach.hpp"
#include "acl_cpp/smtp/mail_body.hpp"
#include "acl_cpp/smtp/mail_message.hpp"

namespace acl
{

mail_message::mail_message(const char* charset /* = "utf-8"*/)
{
	dbuf_ = new dbuf_pool;
	if (charset == NULL || *charset == 0)
		charset = "utf-8";
	charset_ = dbuf_->dbuf_strdup(charset);
	transfer_encoding_ = dbuf_->dbuf_strdup("base64");
	auth_user_ = NULL;
	auth_pass_ = NULL;
	from_ = NULL;
	sender_ = NULL;
	replyto_ = NULL;
	subject_ = NULL;
	body_ = NULL;
	body_len_ = 0;
	filepath_ = NULL;
}

mail_message::~mail_message()
{
	std::vector<mail_attach*>::iterator it;
	for (it = attachments_.begin(); it != attachments_.end(); ++it)
		(*it)->~mail_attach();
	dbuf_->destroy();
}

mail_message& mail_message::set_auth(const char* user, const char* pass)
{
	if (user && *user && pass && *pass)
	{
		auth_user_ = dbuf_->dbuf_strdup(user);
		auth_pass_ = dbuf_->dbuf_strdup(pass);
	}
	return *this;
}

mail_message& mail_message::set_from(const char* from, const char* name)
{
	if (from == NULL || *from == 0)
		return *this;
	from_ = (rfc822_addr*) dbuf_->dbuf_alloc(sizeof(rfc822_addr));
	from_->addr = dbuf_->dbuf_strdup(from);
	if (name && *name)
		from_->comment = dbuf_->dbuf_strdup(name);
	else
		from_->comment = NULL;
	return *this;
}

mail_message& mail_message::set_sender(const char* sender, const char* name)
{
	if (sender == NULL || *sender == 0)
		return *this;
	sender_ = (rfc822_addr*) dbuf_->dbuf_alloc(sizeof(rfc822_addr));
	sender_->addr = dbuf_->dbuf_strdup(sender);
	if (name && *name)
		sender_->comment = dbuf_->dbuf_strdup(name);
	else
		sender_->comment = NULL;
	return *this;
}

mail_message& mail_message::set_replyto(const char* replyto, const char* name)
{
	if (replyto == NULL || *replyto == 0)
		return *this;
	replyto_ = (rfc822_addr*) dbuf_->dbuf_alloc(sizeof(rfc822_addr));
	replyto_->addr = dbuf_->dbuf_strdup(replyto);
	if (name && *name)
		replyto_->comment = dbuf_->dbuf_strdup(name);
	else
		replyto_->comment = NULL;
	return *this;
}

void mail_message::add_addrs(const char* in, std::vector<rfc822_addr*>& out)
{
	rfc822 rfc;
	const std::list<rfc822_addr*>& addrs = rfc.parse_addrs(in, charset_);
	std::list<rfc822_addr*>::const_iterator cit = addrs.begin();
	for (; cit != addrs.end(); ++cit)
	{
		rfc822_addr* addr = (rfc822_addr* )
			dbuf_->dbuf_alloc(sizeof(rfc822_addr));
		if ((*cit)->addr == NULL)
			continue;
		addr->addr = dbuf_->dbuf_strdup((*cit)->addr);
		if ((*cit)->comment)
			addr->comment = dbuf_->dbuf_strdup((*cit)->comment);
		out.push_back(addr);
	}
}

mail_message& mail_message::add_recipients(const char* recipients)
{
	string buf(recipients);
	std::list<string>& tokens = buf.split(" \t;,");
	std::list<string>::const_iterator cit;
	for (cit = tokens.begin(); cit != tokens.end(); ++cit)
		(void) add_to((*cit).c_str());
	return *this;
}

mail_message& mail_message::add_to(const char* to)
{
	if (to && *to)
	{
		add_addrs(to, to_list_);
		add_addrs(to, recipients_);
	}
	return *this;
}

mail_message& mail_message::add_cc(const char* cc)
{
	if (cc && *cc)
	{
		add_addrs(cc, to_list_);
		add_addrs(cc, recipients_);
	}
	return *this;
}

mail_message& mail_message::add_bcc(const char* bcc)
{
	if (bcc && *bcc)
	{
		add_addrs(bcc, to_list_);
		add_addrs(bcc, recipients_);
	}
	return *this;
}

mail_message& mail_message::set_subject(const char* subject)
{
	if (subject && *subject)
		subject_ = dbuf_->dbuf_strdup(subject);
	return *this;
}

mail_message& mail_message::add_header(const char* name, const char* value)
{
	if (name == NULL || *name == 0 || value == NULL || *value == 0)
		return *this;
	char* n = dbuf_->dbuf_strdup(name);
	char* v = dbuf_->dbuf_strdup(value);
	headers_.push_back(std::make_pair(n, v));
	return *this;
}

mail_message& mail_message::set_body(const mail_body* body)
{
	body_ = body;
	return *this;
}

mail_message& mail_message::add_attachment(const char* filepath,
	const char* content_type)
{
	if (filepath == NULL || content_type == NULL)
		return *this;

	char* buf = (char*) dbuf_->dbuf_alloc(sizeof(mail_attach));
	mail_attach* attach = new(buf) mail_attach(filepath,
		content_type, charset_);
	attachments_.push_back(attach);
	return *this;
}

#if defined(_WIN32) || defined(_WIN64)
#include <process.h>
#define PID	_getpid
#else
#include <unistd.h>
#define PID	getpid
#endif // defined(_WIN32) || defined(_WIN64)

bool mail_message::append_addr(ofstream& fp, const char* name,
	const rfc822_addr* addr)
{
	if (fp.format("%s: ", name) == -1)
	{
		logger_error("write to file %s error: %s",
			fp.file_path(), last_serror());
		return false;
	}

	string buf;

	if (addr->comment && rfc2047::encode(addr->comment,
		strlen(addr->comment), &buf, charset_, 'B', false) == false)
	{
		logger_error("rfc2047::encode(%s) error", addr->comment);
		return false;
	}

	if (!buf.empty() && fp.write(buf) == -1)
	{
		logger_error("write comment(%s) error: %s",
			buf.c_str(), last_serror());
		return false;
	}

	if (fp.format("<%s>\r\n", addr->addr) == -1)
	{
		logger_error("write addr(%s) error: %s",
			addr->addr, last_serror());
		return false;
	}

	return true;
}

bool mail_message::append_subject(ofstream& fp, const char* subject)
{
	string buf;
	rfc2047::encode(subject, (int) strlen(subject), &buf,
		charset_, 'B', false);

	if (fp.format("Subject: %s\r\n", buf.c_str()) == -1)
	{
		logger_error("write subject to %s error %s",
			fp.file_path(), last_serror());
		return false;
	}
	return true;
}

bool mail_message::append_message_id(ofstream& fp)
{
	string id;
	id.format("Message-ID: <%lu.%lu.%lu.acl@localhost>\r\n",
		(unsigned long) PID(), thread::thread_self(),
		(unsigned long) time(NULL));
	if (fp.write(id) == -1)
	{
		logger_error("write message_id to %s error: %s",
			fp.file_path(), last_serror());
		return false;
	}
	return true;
}

bool mail_message::append_header(ofstream& fp)
{
	std::vector<std::pair<char*, char*> >::const_iterator cit;
	for (cit = headers_.begin(); cit != headers_.end(); ++cit)
	{
		if (fp.format("%s: %s\r\n", (*cit).first, (*cit).second) == -1)
		{
			logger_error("write one header to %s error: %s",
				fp.file_path(), last_serror());
			return false;
		}
	}

	if (sender_ && !append_addr(fp, "Sender", sender_))
		return false;

	if (replyto_ && !append_addr(fp, "ReplyTo", replyto_))
		return false;

	if (from_ && !append_addr(fp, "From", from_))
		return false;

	if (subject_ && !append_subject(fp, subject_))
		return false;

	if (append_message_id(fp) == false)
		return false;

	return true;
}

bool mail_message::append_multipart_body(ofstream& fp)
{
	if (body_ == NULL)
		return true;

	string body;
	body_->save_to(body);
	if (body.empty())
		return true;

	body.append("\r\n");

	if (fp.format("--%s\r\n", boundary_.c_str()) == -1
		|| fp.write(body) == -1)
	{
		logger_error("write to %s error %s",
			fp.file_path(), last_serror());
		return false;
	}
	return true;
}

void mail_message::create_boundary(const char* id, string& out)
{
	out.format("====_%s_aclPart_%lu_%lu_%lu_====",
		id, (unsigned long) PID(), thread::thread_self(),
		(unsigned long) time(NULL));
}

bool mail_message::append_multipart(ofstream& fp)
{
	const char *prompt = "This is a multi-part message in MIME format.";

	// 创建 MIME 数据唯一分隔符
	create_boundary("0001", boundary_);
	
	// 向邮件头中添加 MIME 相关的信息头
	if (fp.format("Mime-version: 1.0\r\n"
		"Content-Type: multipart/mixed;\r\n"
		"\tcharset=\"%s\";\r\n"
		"\tboundary=\"%s\"\r\n\r\n",
		charset_, boundary_.c_str()) == -1)
	{
		logger_error("write to %s error: %s",
			fp.file_path(), last_serror());
		return false;
	}

	if (fp.format("%s\r\n\r\n", prompt) == -1)
	{
		logger_error("write mime prompt to %s error %s",
			fp.file_path(), last_serror());
		return false;
	}

	// 添加数据体
	if (append_multipart_body(fp) == false)
		return false;

	// 将所有附件内容进行 BASE64 编码后存入目标文件中

	mime_base64 base64(true, false);

	std::vector<mail_attach*>::const_iterator cit;
	for (cit = attachments_.begin(); cit != attachments_.end(); ++cit)
	{
		if (fp.format("--%s\r\n", boundary_.c_str()) == -1)
		{
			logger_error("write boundary to %s error %s",
				fp.file_path(), last_serror());
			return false;
		}

		if ((*cit)->save_to(&base64, fp) == false)
		{
			logger_error("write attachment header to %s error %s",
				fp.file_path(), last_serror());
			return false;
		}
	}

	// 添加最后的分隔符至邮件尾部
	if (fp.format("--%s--\r\n", boundary_.c_str()) == -1)
	{
		logger_error("write boundary end to %s error %s",
			fp.file_path(), last_serror());
		return false;
	}

	return true;
}

bool mail_message::compose(const char* filepath)
{
	ofstream fp;
	if (fp.open_write(filepath) == false)
	{
		logger_error("open %s error: %s", filepath, last_serror());
		return false;
	}

	filepath_ = dbuf_->dbuf_strdup(filepath);

	if (append_header(fp) == false)
		return false;

	if (!attachments_.empty())
		return append_multipart(fp);

	return false;
}

} // namespace acl
