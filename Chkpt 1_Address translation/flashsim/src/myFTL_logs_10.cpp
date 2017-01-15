#include "746FlashSim.h"
#include <set>

template <typename PageType>
class MyFTL : public FlashSim::FTLBase<PageType> {
public:

    uint8_t SSD_SIZE;
    uint8_t PACKAGE_SIZE;
    uint16_t DIE_SIZE;
    uint16_t PLANE_SIZE;
    uint16_t BLOCK_SIZE;
    int BLOCK_ERASES;
    int OVERPROVISIONING;
    size_t RAW_CAPACITY;
    size_t LOG_CAPACITY;
    size_t EMPTY_LOG_BLOCK;
    class LogBlock;
    std::set<size_t> written;
    std::map<size_t, LogBlock*> blockMap;

    class LogBlock {
        uint8_t package;
        uint8_t die;
        uint16_t plane;
        uint16_t block;
        uint16_t emptyPage;
        std::map<uint16_t, uint16_t> pageLog;

    public:

        LogBlock(FlashSim::Address* addr) {
            package = addr->package;
            die = addr->die;
            plane = addr->plane;
            block = addr->block;
            emptyPage = 0;
        }

        std::pair<FlashSim::ExecState, FlashSim::Address> read(
                FlashSim::Address *addr) {
            std::cout << "map size: " << pageLog.size() << std::endl;
            std::map<uint16_t, uint16_t>::iterator it = pageLog.find(addr->page);
            if (it != pageLog.end()) {
                std::cout << "Found a more recent copy (page " << it->second << ") in logs. Return that copy\n";
                return std::make_pair(FlashSim::ExecState::SUCCESS,
                        FlashSim::Address(package, die, plane, block,
                                it->second));
            } else {
                std::cout << "No more recent copy in logs. Return the calculated PA.\n";
                return std::make_pair(FlashSim::ExecState::SUCCESS, *addr);
            }
        }

        std::pair<FlashSim::ExecState, FlashSim::Address> write(
                FlashSim::Address *addr, uint16_t block_size) {
            std::map<uint16_t, uint16_t>::iterator it = pageLog.find(addr->page);
            if (emptyPage == block_size) {
                std::cout << "The mapped block is full!\n";
                return std::make_pair(FlashSim::ExecState::FAILURE,
                        FlashSim::Address(0, 0, 0, 0, 0));
            }
            if (it != pageLog.end()) {
                std::cout << "Update the new page id in map: (" << addr->page << ", "
                        << pageLog[addr->page] << ")->(" << addr->page << ", " << emptyPage << ").\n";
                pageLog[addr->page] = emptyPage;
                return std::make_pair(FlashSim::ExecState::SUCCESS,
                        FlashSim::Address(package, die, plane, block,
                                emptyPage++));
            } else {
                std::cout << "Add new page pair (" << addr->page << ", " << emptyPage << ") to the map.\n";
                pageLog.insert(std::make_pair(addr->page, emptyPage));
                return std::make_pair(FlashSim::ExecState::SUCCESS,
                        FlashSim::Address(package, die, plane, block,
                                emptyPage++));
            }
        }
    };

    // Get the block id given a certain lba.
    size_t getBlockId(size_t lba) {
        return lba / BLOCK_SIZE;
    }

    /*
     * Constructor
     */
    MyFTL(const FlashSim::Configuration* conf) {
        SSD_SIZE = conf->GetInteger("SSD_SIZE");
        PACKAGE_SIZE = conf->GetInteger("PACKAGE_SIZE");
        DIE_SIZE = conf->GetInteger("DIE_SIZE");
        PLANE_SIZE = conf->GetInteger("PLANE_SIZE");
        BLOCK_SIZE = conf->GetInteger("BLOCK_SIZE");
        BLOCK_ERASES = conf->GetInteger("BLOCK_ERASES");
        OVERPROVISIONING = conf->GetInteger("OVERPROVISIONING");
        RAW_CAPACITY = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE
                * BLOCK_SIZE;
        LOG_CAPACITY = RAW_CAPACITY * OVERPROVISIONING / 100;
        EMPTY_LOG_BLOCK = RAW_CAPACITY - LOG_CAPACITY;
        std::cout << "\nSSD_SIZE: " << unsigned(SSD_SIZE) << "\nPACKAGE_SIZE: "
                << unsigned(PACKAGE_SIZE) << "\nDIE_SIZE: " << DIE_SIZE
                << "\nPLANE_SIZE: " << PLANE_SIZE << "\nBLOCK_SIZE: "
                << BLOCK_SIZE << "\nOVERPROVISIONING: " << OVERPROVISIONING
                << "\nRAW_CAPACITY: " << RAW_CAPACITY << "\nEMPTY_LOG_BLOCK: "
                << EMPTY_LOG_BLOCK << std::endl;
    }

    /*
     * Map LBA into PBA.
     * Return FlashSim::Address
     */
    FlashSim::Address mapping(size_t lba) {
        size_t ori_lba = lba;
    	size_t tmp1 = (size_t) PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE * BLOCK_SIZE;
        uint8_t package = lba / tmp1;
        lba = lba % tmp1;

        size_t tmp2 = (size_t) DIE_SIZE * PLANE_SIZE * BLOCK_SIZE;
        uint8_t die = lba / tmp2;
        lba = lba % tmp1;

        uint32_t tmp3 = (uint32_t) PLANE_SIZE * BLOCK_SIZE;
        uint16_t plane = lba / tmp3;
        lba = lba % tmp3;

        uint16_t block = lba / BLOCK_SIZE;
        uint16_t page = lba % BLOCK_SIZE;

        std::cout << "Translate " << ori_lba << " to (" << unsigned(package) << ", " << unsigned(die) << ", "
        		<< plane << ", " << block << ", " << page << ")\n";
        return FlashSim::Address(package, die, plane, block, page);
    }

    /*
     * Destructor - Plase keep it as virtual to allow destroying the
     *              object with base type pointer
     */
    virtual ~MyFTL() {
    }

    /*
     * ReadTranslate() - Translates read address
     *
     * This function translates a physical LBA into an Address object that will
     * be used as the target address of the read operation.
     *
     * If you need to issue extra operations, please use argument func to
     * interact with class Controller
     */
    std::pair<FlashSim::ExecState, FlashSim::Address> ReadTranslate(size_t lba,
            const FlashSim::ExecCallBack<PageType> &func) {
        std::cout << "\nRead: " << lba << "\n";
        if (written.find(lba) == written.end()) {
            // The page has never been written before.
            std::cout << "The page hasn't been written before\n";
            return std::make_pair(FlashSim::ExecState::FAILURE, FlashSim::Address(0, 0, 0, 0, 0));
        }
        FlashSim::Address address = mapping(lba);
        // Invalid lba
        if (lba >= (RAW_CAPACITY - LOG_CAPACITY)) {
            std::cout << "lba out of bound!\n";
            return std::make_pair(FlashSim::ExecState::FAILURE, FlashSim::Address(0, 0, 0, 0, 0));
        }
        typename std::map<size_t, LogBlock*>::iterator it = blockMap.find(getBlockId(lba));
        if (it == blockMap.end()) {
            // If no log-reservation block mapped to this data block, return originally calculated PA
            std::cout << "No block mapped to it. Return the calculated PA\n";
            return std::make_pair(FlashSim::ExecState::SUCCESS, address);
        } else {
            // Check if there is a more recent copy in log-reservation blocks.
            LogBlock* logBlock = it->second;
            std::cout << "Found a log block mapped to block " << it->first << "\n";
            return logBlock->read(&address);
        }
    }

    /*
     * WriteTranslate() - Translates write address
     *
     * Please refer to ReadTranslate()
     */
    std::pair<FlashSim::ExecState, FlashSim::Address> WriteTranslate(size_t lba,
            const FlashSim::ExecCallBack<PageType> &func) {
        std::cout << "\nWrite to: " << lba << "\n";
        // Invalid lba
        if (lba >= (RAW_CAPACITY - LOG_CAPACITY)) {
            std::cout << "lba out of bound!\n";
            return std::make_pair(FlashSim::ExecState::FAILURE, FlashSim::Address(0, 0, 0, 0, 0));
        }
        FlashSim::Address address = mapping(lba);
        if (written.find(lba) != written.end()) {
            // The page is not empty
            typename std::map<size_t, LogBlock*>::iterator it = blockMap.find(lba);
            if (it == blockMap.end()) {
                if (EMPTY_LOG_BLOCK >= RAW_CAPACITY) {
                    // Log-reservation blocks are all full.
                    std::cout << "Log-reservation blocks are full!\n";
                    return std::make_pair(FlashSim::ExecState::FAILURE, FlashSim::Address(0, 0, 0, 0, 0));
                } else {
                    // Map a new log-reservation block to the data block.
                    std::cout << "Map a new log-reservation block to the data block: "
                            << getBlockId(lba) << "->" << getBlockId(EMPTY_LOG_BLOCK) <<std::endl;
                    FlashSim::Address newLogBlock = mapping(EMPTY_LOG_BLOCK);
                    LogBlock *logBlock = new LogBlock(&newLogBlock);
                    EMPTY_LOG_BLOCK += BLOCK_SIZE;
                    blockMap.insert(std::make_pair(getBlockId(lba), logBlock));
                    return logBlock->write(&address, BLOCK_SIZE);
                }
            } else {
                // There's a log-reservation block mapped to this data block.
                LogBlock* logBlock = it->second;
                std::cout << "Found block " << it->first << " mapped to this block.\n";
                return logBlock->write(&address, BLOCK_SIZE);
            }
        } else {
            // The page is empty
            written.insert(lba);
            return std::make_pair(FlashSim::ExecState::SUCCESS, address);
        }
    }
};

/*
 * CreateMyFTL() - Creates class MyFTL object
 *
 * You do not need to modify this
 */
FlashSim::FTLBase<TEST_PAGE_TYPE>* FlashSim::CreateMyFTL(const Configuration* conf) {
    return new MyFTL<TEST_PAGE_TYPE>(conf);
}
