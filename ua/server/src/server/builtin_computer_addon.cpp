/// @author Alexander Rykovanov 2013
/// @email rykovanov.as@gmail.com
/// @brief Test addon wich emulate tcp server addon.
/// @license GNU LGPL
///
/// Distributed under the GNU LGPL License
/// (See accompanying file LICENSE or copy at 
/// http://www.gnu.org/licenses/lgpl.html)
///

#include <opccore/common/addons_core/addon.h>
#include <opc/ua/client/binary_computer.h>
#include <opc/ua/protocol/binary/secure_connection.h>
#include <opc/ua/server/addons/builtin_computer.h>
#include <opc/ua/server/addons/endpoints.h>
#include <opc/ua/server/server.h>

#include <internal/thread.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{

  class BufferedInput : public OpcUa::InputChannel
  {
  public:
    BufferedInput()
      : Running(true)
    {
      Buffer.reserve(4096);
    }

    virtual std::size_t Receive(char* data, std::size_t size)
    {
      std::clog << "Consuming " << size << " bytes of data." << std::endl;
      ThrowIfStopped();


      std::size_t totalConsumedSize = 0;
      while (totalConsumedSize < size)
      {
        std::unique_lock<std::mutex> event(BufferMutex);
        if (Buffer.empty())
        {
          std::clog << "Waiting data from client" << std::endl;
          DataReady.wait(event);
        }
        else if(!event.owns_lock())
        {
          event.lock();
        }
        std::clog << "Buffer contain data from client." << std::endl;
        ThrowIfStopped();
        std::clog << "Client sent data." << std::endl;

        ThrowIfStopped();
        if (Buffer.empty())
        {
          std::clog << "No data in buffer." << std::endl;
          continue;
        }

        const std::size_t sizeToConsume = std::min(size - totalConsumedSize, Buffer.size());
        std::clog << "Consuming " << sizeToConsume << " bytes of data." << std::endl;
        auto endIt = Buffer.begin() + sizeToConsume;
        std::copy(begin(Buffer), endIt, data + totalConsumedSize);
        Buffer.erase(Buffer.begin(), endIt); // TODO make behavoior with round buffer to avoid this.
        totalConsumedSize += sizeToConsume;
      }

      return totalConsumedSize;
    }
  

    void AddBuffer(const char* buf, std::size_t size)
    {
      ThrowIfStopped();
      std::clog << "Client want to send " << size << " bytes of data" << std::endl;
      std::lock_guard<std::mutex> lock(BufferMutex);
      ThrowIfStopped();

      Buffer.insert(Buffer.end(), buf, buf + size);
      std::clog << "Size of buffer " << Buffer.size() << " bytes." << std::endl;
      DataReady.notify_all();
    }

    void Stop()
    {
      Running = false;
      DataReady.notify_all();
    }

  private:
    void ThrowIfStopped()
    {
      if (!Running)
      {
        throw std::logic_error("Conversation through connection stopped.");
      }
    }

  private:
    std::vector<char> Buffer;
    std::atomic<bool> Running;
    std::mutex BufferMutex;
    std::condition_variable DataReady;
  };
  class BufferedIO : public OpcUa::IOChannel
  {
  public:
    BufferedIO(const char* channelID, std::weak_ptr<InputChannel> input, std::weak_ptr<BufferedInput> output)
      : Input(input)
      , Output(output)
      , ID(channelID)
    {
    }

    virtual std::size_t Receive(char* data, std::size_t size)
    {
      std::clog << ID << ": receive data." << std::endl;
      std::shared_ptr<InputChannel> input = Input.lock();
      if (input)
      {
        return input->Receive(data, size);
      }
      return 0;
    }

    virtual void Send(const char* message, std::size_t size)
    {
      std::clog << ID << ": send data." << std::endl;
      std::shared_ptr<BufferedInput> output = Output.lock();
      if (output)
      {
        output->AddBuffer(message, size);
      }
    }

  private:
    std::weak_ptr<InputChannel> Input;
    std::weak_ptr<BufferedInput> Output;
    const std::string ID;
  };


  void Process(std::shared_ptr<OpcUa::Server::IncomingConnectionProcessor> processor, std::shared_ptr<OpcUa::IOChannel> channel)
  {
    processor->Process(channel);
  }


  class BuiltinComputerAddon
    : public OpcUa::Server::BuiltinComputerAddon
    , private OpcUa::Internal::ThreadObserver
  {
  public:
    virtual std::shared_ptr<OpcUa::Remote::Computer> GetComputer() const
    {
      OpcUa::Binary::SecureConnectionParams params;
      params.EndpointUrl = "opc.tcp://localhost:4841";
      params.SecurePolicy = "http://opcfoundation.org/UA/SecurityPolicy#None";
      std::shared_ptr<OpcUa::IOChannel> secureChannel = OpcUa::Binary::CreateSecureChannel(ClientChannel, params);
      return OpcUa::Remote::CreateBinaryComputer(secureChannel);
    }

    ~BuiltinComputerAddon()
    {
      try
      {
        Stop();
      }
      catch (...)
      {
      }
    }

  public: // Common::Addon
    virtual void Initialize(Common::AddonsManager& addons)
    {
      ServerInput.reset(new BufferedInput());
      ClientInput.reset(new BufferedInput());


      ClientChannel.reset(new BufferedIO("Client", ClientInput, ServerInput));
      ServerChannel.reset(new BufferedIO("Server", ServerInput, ClientInput));

      std::shared_ptr<OpcUa::Server::EndpointsAddon> endpoints = Common::GetAddon<OpcUa::Server::EndpointsAddon>(addons, OpcUa::Server::EndpointsAddonID);
      std::shared_ptr<OpcUa::Server::IncomingConnectionProcessor> processor = endpoints->GetProcessor();
      Thread.reset(new OpcUa::Internal::Thread(std::bind(Process, processor, ServerChannel), this));
    }

    virtual void Stop()
    {
      if (ClientInput)
      {
        ClientInput->Stop();
        ServerInput->Stop();
      }

      if (Thread.get())
      {
        Thread->Join();
        Thread.reset();
      }

      ClientInput.reset();
      ServerInput.reset();
    }

  private:
    virtual void OnSuccess()
    {
      ClientInput->Stop();
      std::clog  << "Server thread exited with success." << std::endl;
    }

    virtual void OnError(const std::exception& exc)
    {
      ClientInput->Stop();
      std::clog  << "Server thread exited with error: " << exc.what() << std::endl;
    }


  private:
    std::shared_ptr<BufferedInput> ClientInput;
    std::shared_ptr<BufferedInput> ServerInput;

    std::shared_ptr<OpcUa::IOChannel> ClientChannel;
    std::shared_ptr<OpcUa::IOChannel> ServerChannel;
    std::unique_ptr<OpcUa::Internal::Thread> Thread;
  };

}

extern "C" Common::Addon::UniquePtr CreateAddon()
{
  return Common::Addon::UniquePtr(new ::BuiltinComputerAddon());
}

