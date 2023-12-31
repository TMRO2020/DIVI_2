#ifndef I_DATABASE_WRAPPER_H
#define I_DATABASE_WRAPPER_H
#include <string>
#include <sync.h>

class I_DatabaseWrapper
{
public:
    enum DatabaseStatus
    {
        OK=0,
        RECOVERED_OK=1,
        RECOVERY_FAIL=2
    };
    virtual ~I_DatabaseWrapper(){}
    virtual bool Open() = 0;
    virtual DatabaseStatus Verify(const std::string& walletFilename) = 0;
    virtual void FlushToDisk(const std::string& walletFilename) = 0;
    virtual bool FilenameIsInUse(const std::string& walletFilename) = 0;
    virtual void Lock() = 0;
    virtual void Unlock() = 0;
};
#endif //I_DATABASE_WRAPPER_H