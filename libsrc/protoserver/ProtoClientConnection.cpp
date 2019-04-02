// project includes
#include "ProtoClientConnection.h"

// qt
#include <QTcpSocket>
#include <QHostAddress>
#include <QTimer>
#include <QRgb>

// Hyperion includes
#include <hyperion/Hyperion.h>



ProtoClientConnection::ProtoClientConnection(QTcpSocket* socket, const int &timeout, QObject *parent)
	: QObject(parent)
	, _log(Logger::getInstance("PROTOSERVER"))
	, _socket(socket)
	, _clientAddress(socket->peerAddress().toString())
	, _timeoutTimer(new QTimer(this))
	, _timeout(timeout * 1000)
	, _priority()
	, _hyperion(Hyperion::getInstance())
{
	// timer setup
	_timeoutTimer->setSingleShot(true);
	_timeoutTimer->setInterval(_timeout);
	connect(_timeoutTimer, &QTimer::timeout, this, &ProtoClientConnection::forceClose);

	// connect socket signals
	connect(_socket, &QTcpSocket::readyRead, this, &ProtoClientConnection::readyRead);
	connect(_socket, &QTcpSocket::disconnected, this, &ProtoClientConnection::disconnected);
}

void ProtoClientConnection::readyRead()
{
	_receiveBuffer += _socket->readAll();

	// check if we can read a message size
	if (_receiveBuffer.size() <= 4)
	{
		return;
	}

	// read the message size
	uint32_t messageSize =
			((_receiveBuffer[0]<<24) & 0xFF000000) |
			((_receiveBuffer[1]<<16) & 0x00FF0000) |
			((_receiveBuffer[2]<< 8) & 0x0000FF00) |
			((_receiveBuffer[3]    ) & 0x000000FF);

	// check if we can read a complete message
	if ((uint32_t) _receiveBuffer.size() < messageSize + 4)
	{
		return;
	}

	// read a message
	proto::HyperionRequest message;
	if (!message.ParseFromArray(_receiveBuffer.data() + 4, messageSize))
	{
		sendErrorReply("Unable to parse message");
	}

	// handle the message
	handleMessage(message);

	// remove message data from buffer
	_receiveBuffer = _receiveBuffer.mid(messageSize + 4);
}

void ProtoClientConnection::forceClose()
{
	_socket->close();
}

void ProtoClientConnection::disconnected()
{
	Debug(_log, "Socket Closed");
    _socket->deleteLater();
	_hyperion->clear(_priority);
	emit clientDisconnected();
}

void ProtoClientConnection::handleMessage(const proto::HyperionRequest & message)
{
	switch (message.command())
	{
	case proto::HyperionRequest::COLOR:
		if (!message.HasExtension(proto::ColorRequest::colorRequest))
		{
			sendErrorReply("Received COLOR command without ColorRequest");
			break;
		}
		handleColorCommand(message.GetExtension(proto::ColorRequest::colorRequest));
		break;
	case proto::HyperionRequest::IMAGE:
		if (!message.HasExtension(proto::ImageRequest::imageRequest))
		{
			sendErrorReply("Received IMAGE command without ImageRequest");
			break;
		}
		handleImageCommand(message.GetExtension(proto::ImageRequest::imageRequest));
		break;
	case proto::HyperionRequest::CLEAR:
		if (!message.HasExtension(proto::ClearRequest::clearRequest))
		{
			sendErrorReply("Received CLEAR command without ClearRequest");
			break;
		}
		handleClearCommand(message.GetExtension(proto::ClearRequest::clearRequest));
		break;
	case proto::HyperionRequest::CLEARALL:
		handleClearallCommand();
		break;
	default:
		handleNotImplemented();
	}
}

void ProtoClientConnection::handleColorCommand(const proto::ColorRequest &message)
{
	// extract parameters
	int priority = message.priority();
	int duration = message.has_duration() ? message.duration() : -1;
	ColorRgb color;
	color.red = qRed(message.rgbcolor());
	color.green = qGreen(message.rgbcolor());
	color.blue = qBlue(message.rgbcolor());

	// make sure the prio is registered before setColor()
	if(priority != _priority)
	{
		_hyperion->clear(_priority);
		_hyperion->registerInput(priority, hyperion::COMP_PROTOSERVER, "Proto@"+_clientAddress);
		_priority = priority;
	}

	// set output
	_hyperion->setColor(_priority, color, duration);

	// send reply
	sendSuccessReply();
}

void ProtoClientConnection::handleImageCommand(const proto::ImageRequest &message)
{
	// extract parameters
	int priority = message.priority();
	int duration = message.has_duration() ? message.duration() : -1;
	int width = message.imagewidth();
	int height = message.imageheight();
	const std::string & imageData = message.imagedata();

	// make sure the prio is registered before setInput()
	if(priority != _priority)
	{
		_hyperion->clear(_priority);
		_hyperion->registerInput(priority, hyperion::COMP_PROTOSERVER, "Proto@"+_clientAddress);
		_priority = priority;
	}

	// check consistency of the size of the received data
	if ((int) imageData.size() != width*height*3)
	{
		sendErrorReply("Size of image data does not match with the width and height");
		return;
	}

	// create ImageRgb
	Image<ColorRgb> image(width, height);
	memcpy(image.memptr(), imageData.c_str(), imageData.size());

	_hyperion->setInputImage(_priority, image, duration);

	// send reply
	sendSuccessReply();
}


void ProtoClientConnection::handleClearCommand(const proto::ClearRequest &message)
{
	// extract parameters
	int priority = message.priority();

	// clear priority
	_hyperion->clear(priority);
	// send reply
	sendSuccessReply();
}

void ProtoClientConnection::handleClearallCommand()
{
	// clear priority
	_hyperion->clearall();

	// send reply
	sendSuccessReply();
}


void ProtoClientConnection::handleNotImplemented()
{
	sendErrorReply("Command not implemented");
}

void ProtoClientConnection::sendMessage(const google::protobuf::Message &message)
{
	std::string serializedReply = message.SerializeAsString();
	uint32_t size = serializedReply.size();
	uint8_t sizeData[] = {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)};
	_socket->write((const char *) sizeData, sizeof(sizeData));
	_socket->write(serializedReply.data(), serializedReply.length());
	_socket->flush();
}

void ProtoClientConnection::sendSuccessReply()
{
	// create reply
	proto::HyperionReply reply;
	reply.set_type(proto::HyperionReply::REPLY);
	reply.set_success(true);

	// send reply
	sendMessage(reply);
}

void ProtoClientConnection::sendErrorReply(const std::string &error)
{
	// create reply
	proto::HyperionReply reply;
	reply.set_type(proto::HyperionReply::REPLY);
	reply.set_success(false);
	reply.set_error(error);

	// send reply
	sendMessage(reply);
}
