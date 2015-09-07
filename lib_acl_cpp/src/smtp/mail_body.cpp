#include "acl_stdafx.hpp"
#include "acl_cpp/stdlib/log.hpp"
#include "acl_cpp/mime/mime_base64.hpp"
#include "acl_cpp/mime/mime_code.hpp"
#include "acl_cpp/mime/mime_quoted_printable.hpp"
#include "acl_cpp/mime/mime_uucode.hpp"
#include "acl_cpp/mime/mime_xxcode.hpp"
#include "acl_cpp/smtp/mail_attach.hpp"
#include "acl_cpp/smtp/mail_message.hpp"
#include "acl_cpp/smtp/mail_body.hpp"

namespace acl {

mail_body::mail_body(const char* charset /* = "utf-8" */,
	const char* encoding /* = "base64" */)
	: charset_(charset)
	, transfer_encoding_(encoding)
{
	if (transfer_encoding_.compare("base64", false) == 0)
		coder_ = NEW mime_base64(true, true);
	else if (transfer_encoding_.compare("qp", false) == 0)
		coder_ = NEW mime_quoted_printable(true, true);
	else if (transfer_encoding_.compare("uucode", false) == 0)
		coder_ = NEW mime_uucode(true, true);
	else if (transfer_encoding_.compare("xxcode", false) == 0)
		coder_ = NEW mime_xxcode(true, true);
	else
		coder_ = NULL;

	html_ = NULL;
	hlen_ = 0;
	text_ = NULL;
	tlen_ = 0;
	attachments_ = NULL;
	mime_stype_ = MIME_STYPE_OTHER;
}

mail_body::~mail_body()
{
	delete coder_;
}

mail_body& mail_body::set_html(const char* html, size_t len)
{
	html_ = html;
	hlen_ = len;
	mime_stype_ = MIME_STYPE_HTML;

	return *this;
}

mail_body& mail_body::set_text(const char* text, size_t len)
{
	text_ = text;
	tlen_ = len;
	mime_stype_ = MIME_STYPE_PLAIN;

	return *this;
}

mail_body& mail_body::set_alternative(const char* html, size_t hlen,
	const char* text, size_t tlen)
{
	html_ = html;
	hlen_ = hlen;
	text_ = text;
	tlen_ = tlen;
	mime_stype_ = MIME_STYPE_ALTERNATIVE;

	return *this;
}

mail_body& mail_body::set_relative(const char* html, size_t hlen,
	const char* text, size_t tlen,
	const std::vector<mail_attach*>& attachments)
{
	html_ = html;
	hlen_ = hlen;
	text_ = text;
	tlen_ = tlen;
	attachments_ = &attachments;
	mime_stype_ = MIME_STYPE_RELATED;

	return *this;
}

bool mail_body::build(const char* in, size_t len, string& out) const
{
	out.format_append("Content-Type: %s;\r\n", content_type_.c_str());
	out.format_append("\tcharset=\"%s\"\r\n", charset_.c_str());
	out.format_append("Content-Transfer-Encoding: %s\r\n\r\n",
		transfer_encoding_.c_str());
	coder_->encode_update(in, (int) len, &out);
	coder_->encode_finish(&out);
	return true;
}

bool mail_body::save_to(string& out) const
{
	switch (mime_stype_)
	{
	case MIME_STYPE_HTML:
		return save_html(html_, hlen_, out);
	case MIME_STYPE_PLAIN:
		return save_text(text_, tlen_, out);
	case MIME_STYPE_ALTERNATIVE:
		return save_alternative(html_, hlen_, text_, tlen_, out);
	case MIME_STYPE_RELATED:
		acl_assert(attachments_);
		return save_relative(html_, hlen_, text_, tlen_,
			*attachments_, out);
	default:
		logger_error("unknown mime_stype: %s", mime_stype_);
		return false;
	}
}

void mail_body::set_content_type(const char* content_type)
{
	content_type_.format(content_type);
	ctype_.parse(content_type_);
}

bool mail_body::save_html(const char* html, size_t len, string& out) const
{
	if (!html || !len)
	{
		logger_error("invalid input!");
		return false;
	}

	const_cast<mail_body*>(this)->set_content_type("text/html");
	return build(html, len, out);
}

bool mail_body::save_text(const char* text, size_t len, string& out) const
{
	if (!text || !len)
	{
		logger_error("invalid input!");
		return false;
	}

	const_cast<mail_body*>(this)->set_content_type("text/plain");
	return build(text, len, out);
}

bool mail_body::save_relative(const char* html, size_t hlen,
	const char* text, size_t tlen,
	const std::vector<mail_attach*>& attachments,
	string& out) const
{
	if (!html || !hlen || !text || !tlen || attachments.empty())
	{
		logger_error("invalid input!");
		return false;
	}

	mail_message::create_boundary("0002",
		const_cast<mail_body*>(this)->boundary_);
	string ctype;
	ctype.format("multipart/relative;\r\n"
		"\ttype=\"multipart/alternative\";\r\n"
		"\tboundary=\"%s\"", boundary_.c_str());
	const_cast<mail_body*>(this)->set_content_type(ctype);

	out.format_append("Content-Type: %s\r\n\r\n", content_type_.c_str());
	out.append("\r\n");
	out.format_append("--%s\r\n", boundary_.c_str());

	// 递归一层，调用生成 alternative 格式数据
	mail_body body(charset_.c_str(), transfer_encoding_.c_str());
	bool ret = body.save_alternative(html, hlen, text, tlen, out);
	if (ret == false)
		return ret;

	out.append("\r\n");

	std::vector<mail_attach*>::const_iterator cit;
	for (cit = attachments.begin(); cit != attachments.end(); ++cit)
	{
		out.format_append("--%s\r\n", boundary_.c_str());
		if ((*cit)->save_to(coder_, out) == false)
			return false;
		out.append("\r\n");
	}

	out.format_append("\r\n--%s--\r\n", boundary_.c_str());

	return true;
}

bool mail_body::save_alternative(const char* html, size_t hlen,
	const char* text, size_t tlen, string& out) const
{
	if (!html || !hlen || !text || !tlen)
	{
		logger_error("invalid input!");
		return false;
	}

	mail_message::create_boundary("0003",
		const_cast<mail_body*>(this)->boundary_);
	string ctype;
	ctype.format("multipart/alternative;\r\n"
		"\tboundary=\"%s\"", boundary_.c_str());
	const_cast<mail_body*>(this)->set_content_type(ctype);

	out.format_append("Content-Type: %s\r\n\r\n", content_type_.c_str());
	out.format_append("--%s\r\n", boundary_.c_str());
	out.format_append("Content-Type: text/plain;\r\n");
	out.format_append("\tcharset=\"%s\"\r\n", charset_.c_str());
	out.format_append("Content-Transfer-Encoding: %s\r\n\r\n",
		transfer_encoding_.c_str());
	coder_->encode_update(text, (int) tlen, &out);
	coder_->encode_finish(&out);
	out.append("\r\n\r\n");

	out.format_append("--%s\r\n", boundary_.c_str());
	out.format_append("Content-Type: text/html;\r\n");
	out.format_append("\tcharset=\"%s\"\r\n", charset_.c_str());
	out.format_append("Content-Transfer-Encoding: %s\r\n\r\n",
		transfer_encoding_.c_str());
	coder_->encode_update(html, (int) hlen, &out);
	coder_->encode_finish(&out);
	out.append("\r\n\r\n");

	out.format_append("--%s--\r\n", boundary_.c_str());

	return true;
}

} // namespace acl
