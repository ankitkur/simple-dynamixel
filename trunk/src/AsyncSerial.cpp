/*
 * File:   AsyncSerial.cpp
 * Author: Terraneo Federico
 * Distributed under the Boost Software License, Version 1.0.
 * Created on September 7, 2009, 10:46 AM
 *
 * v1.02: Fixed a bug in BufferedAsyncSerial: Using the default constructor
 * the callback was not set up and reading didn't work.
 *
 * v1.01: Fixed a bug that did not allow to reopen a closed serial port.
 *
 * v1.00: First release.
 *
 * IMPORTANT:
 * On Mac OS X boost asio's serial ports have bugs, and the usual implementation
 * of this class does not work. So a workaround class was written temporarily,
 * until asio (hopefully) will fix Mac compatibility for serial ports.
 * 
 * Please note that unlike said in the documentation on OS X until asio will
 * be fixed serial port *writes* are *not* asynchronous, but at least
 * asynchronous *read* works.
 * In addition the serial port open ignores the following options: parity,
 * character size, flow, stop bits, and defaults to 8N1 format.
 * I know it is bad but at least it's better than nothing.
 *
 */

#include "AsyncSerial.h"

#include <string>
#include <algorithm>
#include <iostream>
#include <boost/bind.hpp>
#include <IOKit/serial/ioss.h>


using namespace std;
using namespace boost;

//
//Class AsyncSerial
//

#ifndef __APPLE__

class AsyncSerialImpl: private boost::noncopyable
{
public:
    AsyncSerialImpl(): io(), port(io), backgroundThread(), open(false),
            error(false) {}

    boost::asio::io_service io; ///< Io service object
    boost::asio::serial_port port; ///< Serial port object
    boost::thread backgroundThread; ///< Thread that runs read/write operations
    bool open; ///< True if port open
    bool error; ///< Error flag
    mutable boost::mutex errorMutex; ///< Mutex for access to error

    /// Data are queued here before they go in writeBuffer
    std::vector<char> writeQueue;
    boost::shared_array<char> writeBuffer; ///< Data being written
    size_t writeBufferSize; ///< Size of writeBuffer
    boost::mutex writeQueueMutex; ///< Mutex for access to writeQueue
    char readBuffer[AsyncSerial::readBufferSize]; ///< data being read

    /// Read complete callback
    boost::function<void (const char*, size_t)> callback;
};

AsyncSerial::AsyncSerial(): pimpl(new AsyncSerialImpl)
{

}

AsyncSerial::AsyncSerial(const std::string& devname, unsigned int baud_rate,
        asio::serial_port_base::parity opt_parity,
        asio::serial_port_base::character_size opt_csize,
        asio::serial_port_base::flow_control opt_flow,
        asio::serial_port_base::stop_bits opt_stop)
        : pimpl(new AsyncSerialImpl)
{
    open(devname,baud_rate,opt_parity,opt_csize,opt_flow,opt_stop);
}

void AsyncSerial::open(const std::string& devname, unsigned int baud_rate,
        asio::serial_port_base::parity opt_parity,
        asio::serial_port_base::character_size opt_csize,
        asio::serial_port_base::flow_control opt_flow,
        asio::serial_port_base::stop_bits opt_stop)
{
    if(isOpen()) close();

    std::cout << "set baudrate: " << baud_rate << std::endl;

    setErrorStatus(true);//If an exception is thrown, error_ remains true
    pimpl->port.open(devname);
    pimpl->port.set_option(asio::serial_port_base::baud_rate(baud_rate));
    pimpl->port.set_option(opt_parity);
    pimpl->port.set_option(opt_csize);
    pimpl->port.set_option(opt_flow);
    pimpl->port.set_option(opt_stop);

    //This gives some work to the io_service before it is started
    pimpl->io.post(boost::bind(&AsyncSerial::doRead, this));

    thread t(boost::bind(&asio::io_service::run, &pimpl->io));
    pimpl->backgroundThread.swap(t);
    setErrorStatus(false);//If we get here, no error
    pimpl->open=true; //Port is now open
}

bool AsyncSerial::isOpen() const
{
    return pimpl->open;
}

bool AsyncSerial::errorStatus() const
{
    lock_guard<mutex> l(pimpl->errorMutex);
    return pimpl->error;
}

void AsyncSerial::close()
{
    if(!isOpen()) return;

    pimpl->open=false;
    pimpl->io.post(boost::bind(&AsyncSerial::doClose, this));
    pimpl->backgroundThread.join();
    pimpl->io.reset();
    if(errorStatus())
    {
        throw(boost::system::system_error(boost::system::error_code(),
                "Error while closing the device"));
    }
}

void AsyncSerial::write(const char *data, size_t size)
{
    {
        lock_guard<mutex> l(pimpl->writeQueueMutex);
        pimpl->writeQueue.insert(pimpl->writeQueue.end(),data,data+size);
    }
    pimpl->io.post(boost::bind(&AsyncSerial::doWrite, this));
}

void AsyncSerial::write(const std::vector<char>& data)
{
    {
        lock_guard<mutex> l(pimpl->writeQueueMutex);
        pimpl->writeQueue.insert(pimpl->writeQueue.end(),data.begin(),
                data.end());
    }
    pimpl->io.post(boost::bind(&AsyncSerial::doWrite, this));
}

void AsyncSerial::writeString(const std::string& s)
{
    {
        lock_guard<mutex> l(pimpl->writeQueueMutex);
        pimpl->writeQueue.insert(pimpl->writeQueue.end(),s.begin(),s.end());
    }
    pimpl->io.post(boost::bind(&AsyncSerial::doWrite, this));
}

AsyncSerial::~AsyncSerial()
{
    if(isOpen())
    {
        try {
            close();
        } catch(...)
        {
            //Don't throw from a destructor
        }
    }
}

void AsyncSerial::doRead()
{
    pimpl->port.async_read_some(asio::buffer(pimpl->readBuffer,readBufferSize),
            boost::bind(&AsyncSerial::readEnd,
            this,
            asio::placeholders::error,
            asio::placeholders::bytes_transferred));
}

void AsyncSerial::readEnd(const boost::system::error_code& error,
        size_t bytes_transferred)
{
    if(error)
    {
        #ifdef __APPLE__
        if(error.value()==45)
        {
            //Bug on OS X, it might be necessary to repeat the setup
            //http://osdir.com/ml/lib.boost.asio.user/2008-08/msg00004.html
            doRead();
            return;
        }
        #endif //__APPLE__
        //error can be true even because the serial port was closed.
        //In this case it is not a real error, so ignore
        if(isOpen())
        {
            doClose();
            setErrorStatus(true);
        }
    } else {
        if(pimpl->callback) pimpl->callback(pimpl->readBuffer,
                bytes_transferred);
        doRead();
    }
}

void AsyncSerial::doWrite()
{
    //If a write operation is already in progress, do nothing
    if(pimpl->writeBuffer==0)
    {
        lock_guard<mutex> l(pimpl->writeQueueMutex);
        pimpl->writeBufferSize=pimpl->writeQueue.size();
        pimpl->writeBuffer.reset(new char[pimpl->writeQueue.size()]);
        copy(pimpl->writeQueue.begin(),pimpl->writeQueue.end(),
                pimpl->writeBuffer.get());
        pimpl->writeQueue.clear();
        async_write(pimpl->port,asio::buffer(pimpl->writeBuffer.get(),
                pimpl->writeBufferSize),
                boost::bind(&AsyncSerial::writeEnd, this, asio::placeholders::error));
    }
}

void AsyncSerial::writeEnd(const boost::system::error_code& error)
{
    if(!error)
    {
        lock_guard<mutex> l(pimpl->writeQueueMutex);
        if(pimpl->writeQueue.empty())
        {
            pimpl->writeBuffer.reset();
            pimpl->writeBufferSize=0;
            
            return;
        }
        pimpl->writeBufferSize=pimpl->writeQueue.size();
        pimpl->writeBuffer.reset(new char[pimpl->writeQueue.size()]);
        copy(pimpl->writeQueue.begin(),pimpl->writeQueue.end(),
                pimpl->writeBuffer.get());
        pimpl->writeQueue.clear();
        async_write(pimpl->port,asio::buffer(pimpl->writeBuffer.get(),
                pimpl->writeBufferSize),
                boost::bind(&AsyncSerial::writeEnd, this, asio::placeholders::error));
    } else {
        setErrorStatus(true);
        doClose();
    }
}

void AsyncSerial::doClose()
{
    boost::system::error_code ec;
    pimpl->port.cancel(ec);
    if(ec) setErrorStatus(true);
    pimpl->port.close(ec);
    if(ec) setErrorStatus(true);
}

void AsyncSerial::setErrorStatus(bool e)
{
    lock_guard<mutex> l(pimpl->errorMutex);
    pimpl->error=e;
}

void AsyncSerial::setReadCallback(const boost::function<void (const char*, size_t)>& callback)
{
    pimpl->callback=callback;
}

void AsyncSerial::clearReadCallback()
{
    pimpl->callback.clear();
}

#else //__APPLE__

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

class AsyncSerialImpl: private boost::noncopyable
{
public:
    AsyncSerialImpl(): backgroundThread(), open(false), error(false) {}

    boost::thread backgroundThread; ///< Thread that runs read operations
    bool open; ///< True if port open
    bool error; ///< Error flag
    mutable boost::mutex errorMutex; ///< Mutex for access to error

    int fd; ///< File descriptor for serial port
    
    char readBuffer[AsyncSerial::readBufferSize]; ///< data being read

    /// Read complete callback
    boost::function<void (const char*, size_t)> callback;
};

AsyncSerial::AsyncSerial(): pimpl(new AsyncSerialImpl)
{

}

AsyncSerial::AsyncSerial(const std::string& devname, unsigned int baud_rate,
        asio::serial_port_base::parity opt_parity,
        asio::serial_port_base::character_size opt_csize,
        asio::serial_port_base::flow_control opt_flow,
        asio::serial_port_base::stop_bits opt_stop)
        : pimpl(new AsyncSerialImpl)
{
    open(devname,baud_rate,opt_parity,opt_csize,opt_flow,opt_stop);
}

void AsyncSerial::open(const std::string& devname, unsigned int baud_rate,
        asio::serial_port_base::parity opt_parity,
        asio::serial_port_base::character_size opt_csize,
        asio::serial_port_base::flow_control opt_flow,
        asio::serial_port_base::stop_bits opt_stop)
{
    if(isOpen()) close();

    setErrorStatus(true);//If an exception is thrown, error remains true
    
    struct termios new_attributes;
    speed_t speed;
    int status;
    
/*
    // Open port
    pimpl->fd=::open(devname.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (pimpl->fd<0) throw(boost::system::system_error(
            boost::system::error_code(),"Failed to open port"));

*/
    struct termios options;

    // open the serial like POSIX C
    pimpl->fd = ::open(devname.c_str(),
                     O_RDWR |
                     O_NOCTTY |
                     O_NONBLOCK );

    // block non-root users from using this port
    status = ioctl(pimpl->fd, TIOCEXCL);
    if(status < 0)
        std::cout << "ioctl(pimpl->fd, TIOCEXCL)" << std::endl;

    // clear the O_NONBLOCK flag, so that read() will
    //   block and wait for data.
    status = fcntl(pimpl->fd, F_SETFL, 0);
    if(status < 0)
        std::cout << "fcntl(pimpl->fd, F_SETFL, 0)" << std::endl;

    // grab the options for the serial port
    status = tcgetattr(pimpl->fd, &options);
    if(status < 0)
        std::cout << "tcgetattr(pimpl->fd, &options)" << std::endl;

    // setting raw-mode allows the use of tcsetattr() and ioctl()
    /*
    options.c_iflag = IGNBRK;
    options.c_oflag = 0;
    options.c_lflag = 0;
    options.c_cflag = (CS8 | CREAD | CLOCAL);//8 data bit,Enable receiver,Ignore modem

    options.c_cc[VMIN]=1;// Minimum number of characters to read before returning error
    options.c_cc[VTIME]=1;// Set timeouts in tenths of second
    */
    options.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    options.c_oflag &= ~OPOST;
    options.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    options.c_cflag &= ~(CSIZE | PARENB);
    options.c_cflag |= CS8;


    status = tcsetattr(pimpl->fd, TCSANOW, &options);
    if(status < 0)
        std::cout << "tcsetattr(pimpl->fd, TCSANOW, &options)" << std::endl;

    // specify any arbitrary baud rate
    int new_baud = static_cast<int> (baud_rate);
    status = ioctl(pimpl->fd, IOSSIOSPEED, &new_baud);
    if(status < 0)
        std::cout << "ioctl(pimpl->fd, IOSSIOSPEED, &new_baud)" << std::endl;


    
/*
    ioctl(pimpl->fd, TIOCEXCL);///////////////////////////////////

    // Set Port parameters.
    status=tcgetattr(pimpl->fd,&new_attributes);
    if(status<0  || !isatty(pimpl->fd))
    {
        ::close(pimpl->fd);
        throw(boost::system::system_error(
                    boost::system::error_code(),"Device is not a tty"));
    }
    new_attributes.c_iflag = IGNBRK;
    new_attributes.c_oflag = 0;
    new_attributes.c_lflag = 0;
    new_attributes.c_cflag = (CS8 | CREAD | CLOCAL);//8 data bit,Enable receiver,Ignore modem

    new_attributes.c_cc[VMIN]=1;// Minimum number of characters to read before returning error
    new_attributes.c_cc[VTIME]=1;// Set timeouts in tenths of second

    bool customBaudRate = false;
    // Set baud rate
    switch(baud_rate)
    {
        case 50:speed= B50; break;
        case 75:speed= B75; break;
        case 110:speed= B110; break;
        case 134:speed= B134; break;
        case 150:speed= B150; break;
        case 200:speed= B200; break;
        case 300:speed= B300; break;
        case 600:speed= B600; break;
        case 1200:speed= B1200; break;
        case 1800:speed= B1800; break;
        case 2400:speed= B2400; break;
        case 4800:speed= B4800; break;
        case 9600:speed= B9600; break;
        case 19200:speed= B19200; break;
        case 38400:speed= B38400; break;
        case 57600:speed= B57600; break;
        case 115200:speed= B115200; break;
        case 230400:speed= B230400; break;

        #ifdef B460800
          case 460800: speed = B460800; break;
        #endif
        #ifdef B921600
          case 921600: speed = B921600; break;
        #endif
        #ifdef B1000000
          case 1000000: speed = B1000000; break;
        #endif
        #ifdef B1152000
          case 1152000: speed = B1152000; break;
        #endif
        #ifdef B1500000
          case 1500000: speed = B1500000; break;
        #endif
        #ifdef B2000000
          case 2000000: speed = B2000000; break;
        #endif
        #ifdef B2500000
          case 2500000: speed = B2500000; break;
        #endif
        #ifdef B3000000
          case 3000000: speed = B3000000; break;
        #endif
        #ifdef B3500000
          case 3500000: speed = B3500000; break;
        #endif
        #ifdef B4000000
          case 4000000: speed = B4000000; break;
        #endif
          default:
            // set custom baudrate
            customBaudRate = true;
            //#define IOSSIOSPEED _IOW('T', 2, speed_t)

               std::cout << "XX set baudrate: x" << baud_rate << std::endl;

                cfmakeraw(&new_attributes);

                int new_baud = static_cast<int> (baud_rate);
                if (ioctl (pimpl->fd, IOSSIOSPEED, &new_baud, 1) < 0)
                {
                    ::close(pimpl->fd);
                    throw(boost::system::system_error(
                                boost::system::error_code(),"Unsupported baud rate"));
                }

            break;
    }



    if (customBaudRate == false)
    {
        cfsetospeed(&new_attributes,(speed_t)baud_rate);
        cfsetispeed(&new_attributes,(speed_t)baud_rate);
    }



    //Make changes effective
    status=tcsetattr(pimpl->fd, TCSANOW, &new_attributes);
    if(status<0)
    {
        ::close(pimpl->fd);
        throw(boost::system::system_error(
                    boost::system::error_code(),"Can't set port attributes"));
    }

    //These 3 lines clear the O_NONBLOCK flag
    status=fcntl(pimpl->fd, F_GETFL, 0);
    if(status!=-1) fcntl(pimpl->fd, F_SETFL, status & ~O_NONBLOCK);
*/




    setErrorStatus(false);//If we get here, no error
    pimpl->open=true; //Port is now open

    thread t(bind(&AsyncSerial::doRead, this));
    pimpl->backgroundThread.swap(t);
}

bool AsyncSerial::isOpen() const
{
    return pimpl->open;
}

bool AsyncSerial::errorStatus() const
{
    lock_guard<mutex> l(pimpl->errorMutex);
    return pimpl->error;
}

void AsyncSerial::close()
{
    if(!isOpen()) return;

    pimpl->open=false;

    ::close(pimpl->fd); //The thread waiting on I/O should return

    pimpl->backgroundThread.join();
    if(errorStatus())
    {
        throw(boost::system::system_error(boost::system::error_code(),
                "Error while closing the device"));
    }
}

void AsyncSerial::write(const char *data, size_t size)
{
    if(::write(pimpl->fd,data,size)!=size) setErrorStatus(true);
}

void AsyncSerial::write(const std::vector<char>& data)
{
    if(::write(pimpl->fd,&data[0],data.size())!=data.size())
        setErrorStatus(true);
}

void AsyncSerial::writeString(const std::string& s)
{
    if(::write(pimpl->fd,&s[0],s.size())!=s.size()) setErrorStatus(true);
}

AsyncSerial::~AsyncSerial()
{
    if(isOpen())
    {
        try {
            close();
        } catch(...)
        {
            //Don't throw from a destructor
        }
    }
}

void AsyncSerial::doRead()
{
    //Read loop in spawned thread
    for(;;)
    {
        int received=::read(pimpl->fd,pimpl->readBuffer,readBufferSize);
        if(received<0)
        {
            if(isOpen()==false) return; //Thread interrupted because port closed
            else {
                setErrorStatus(true);
                continue;
            }
        }
        if(pimpl->callback) pimpl->callback(pimpl->readBuffer, received);
    }
}

void AsyncSerial::readEnd(const boost::system::error_code& error,
        size_t bytes_transferred)
{
    //Not used
}

void AsyncSerial::doWrite()
{
    //Not used
}

void AsyncSerial::writeEnd(const boost::system::error_code& error)
{
    //Not used
}

void AsyncSerial::doClose()
{
    //Not used
}

void AsyncSerial::setErrorStatus(bool e)
{
    lock_guard<mutex> l(pimpl->errorMutex);
    pimpl->error=e;
}

void AsyncSerial::setReadCallback(const
        function<void (const char*, size_t)>& callback)
{
    pimpl->callback=callback;
}

void AsyncSerial::clearReadCallback()
{
    pimpl->callback.clear();
}

#endif //__APPLE__

//
//Class CallbackAsyncSerial
//

CallbackAsyncSerial::CallbackAsyncSerial(): AsyncSerial()
{

}

CallbackAsyncSerial::CallbackAsyncSerial(const std::string& devname,
        unsigned int baud_rate,
        asio::serial_port_base::parity opt_parity,
        asio::serial_port_base::character_size opt_csize,
        asio::serial_port_base::flow_control opt_flow,
        asio::serial_port_base::stop_bits opt_stop)
        :AsyncSerial(devname,baud_rate,opt_parity,opt_csize,opt_flow,opt_stop)
{

}

void CallbackAsyncSerial::setCallback(const
        boost::function<void (const char*, size_t)>& callback)
{
    setReadCallback(callback);
}

void CallbackAsyncSerial::clearCallback()
{
    clearReadCallback();
}

CallbackAsyncSerial::~CallbackAsyncSerial()
{
    clearReadCallback();
}
