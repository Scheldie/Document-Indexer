#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <locale>
#include <clocale>
#include <codecvt>
#include <thread>
#include <chrono>


// Для filesystem в C++14
#include <boost/filesystem.hpp>

// Для работы с PDF
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-global.h>
#include <poppler/cpp/poppler-page-renderer.h>
#include <iconv.h>

// CLucene headers
#include <CLucene.h>
#include <CLucene/analysis/standard/StandardAnalyzer.h>
#include <CLucene/analysis/AnalysisHeader.h>
#include <CLucene/document/Document.h>
#include <CLucene/document/Field.h>
#include <CLucene/index/IndexWriter.h>
#include <CLucene/index/IndexReader.h>
#include <CLucene/index/Term.h>
#include <CLucene/search/IndexSearcher.h>
#include <CLucene/search/TermQuery.h>
#include <CLucene/store/Directory.h>
#include <CLucene/store/FSDirectory.h>
#include "Clucene/analysis/LanguageBasedAnalyzer.h"


namespace fs = boost::filesystem;

using namespace lucene;
using lucene::index::IndexWriter;
using lucene::index::IndexReader;
using lucene::document::Document;
using lucene::document::Field;
using lucene::search::IndexSearcher;
using lucene::search::TermQuery;
using lucene::search::Hits;
using lucene::store::Directory;
using lucene::store::FSDirectory;
using lucene::analysis::standard::StandardAnalyzer;

#define UNICODE
#define _UNICODE

// Упрощенный конвертер для совместимости с CLucene
const TCHAR* toTCHAR(const std::string& str) {
#ifdef _UNICODE
    static thread_local std::wstring wideStr;
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    wideStr = converter.from_bytes(str);
    return wideStr.c_str();
#else
    return str.c_str();
#endif
}

std::string TCHAR_TO_STRING(const TCHAR* str) {
    if (str == nullptr) return "";
#ifdef _UNICODE
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(str);
#else
    return str;
#endif
}
std::wstring utf8_to_wstring(const std::string& str) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.from_bytes(str);
}
std::string wstring_to_utf8(const std::wstring& wstr) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    return conv.to_bytes(wstr);
}
std::string safe_convert_encoding(const std::string& input, const char* from, const char* to) {
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) {
        std::cerr << "iconv_open failed: " << strerror(errno) << std::endl;
        return input;
    }

    size_t in_bytes = input.size();
    size_t out_bytes = in_bytes * 4;
    std::string output(out_bytes, '\0');

    char* in_ptr = const_cast<char*>(input.data());
    char* out_ptr = &output[0];

    if (iconv(cd, &in_ptr, &in_bytes, &out_ptr, &out_bytes) == (size_t)-1) {
        std::cerr << "iconv failed: " << strerror(errno) << std::endl;
        iconv_close(cd);
        return input;
    }

    iconv_close(cd);
    output.resize(output.size() - out_bytes);
    return output;
}

std::vector<std::pair<int, std::string>> extractTextWithPagesFromFile(const fs::path& filePath) {
    std::vector<std::pair<int, std::string>> pagesContent;

    if (filePath.extension() == ".txt") {
        try {
            std::ifstream file(filePath.string(), std::ios::binary);
            if (file) {
                std::string content((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
                if (!content.empty()) {
                    pagesContent.emplace_back(0, content);
                }
            }
        } catch (...) {
            std::cerr << "TXT read error: " << filePath << std::endl;
        }
    }
    else if (filePath.extension() == ".pdf") {
        std::cout << "\nProcessing PDF: " << filePath << std::endl;

        try {
            auto doc = poppler::document::load_from_file(filePath.string());
            if (!doc) {
                std::cerr << "ERROR: Document is null" << std::endl;
                return pagesContent;
            }



            // Вывод метаданных
            std::cout << "PDF Metadata:" << std::endl;

            std::cout << "  Pages: " << doc->pages() << std::endl;

            std::cout << "  Processing pages " << "(0 - " << doc->pages() << ")" << std::endl;
            for (int i = 0; i < doc->pages(); ++i) {
                std::cout << ".";
                try {
                    auto page = doc->create_page(i);
                    if (!page) {
                        std::cerr << "  WARNING: Page " << i << " is null" << std::endl;
                        continue;
                    }

                    auto ustr = page->text();
                    if (ustr.empty()) {
                        std::cerr << "  WARNING: Empty text on page " << i << std::endl;
                        continue;
                    }

                    // Вариант 1: UTF-8
                    try {
                        auto utf8 = ustr.to_utf8();
                        std::string content(utf8.begin(), utf8.end());
                        if (!content.empty()) {
                            pagesContent.emplace_back(i, content);
                            std::wcout << L"  SUCCESS UTF-8: " << utf8_to_wstring(content.substr(0, 50)) << std::endl;
                            continue;
                        }
                    } catch (...) {}

                    // Вариант 2: Latin1 как fallback
                    try {
                        auto latin = ustr.to_latin1();
                        std::string content(latin.begin(), latin.end());
                        if (!content.empty()) {
                            pagesContent.emplace_back(i, content);
                            std::cout << "  SUCCESS Latin1: " << content.substr(0, 50) << std::endl;
                            continue;
                        }
                    } catch (...) {}

                    std::cerr << "  ERROR: All conversion methods failed" << std::endl;
                }
                catch (const std::exception& e) {
                    std::cerr << "  EXCEPTION on page " << i << ": " << e.what() << std::endl;
                }
            }
            std::cout << std::endl;
        }
        catch (const std::exception& e) {
            std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        }
    }
    return pagesContent;
}

// Индексация файлов
void indexFiles(const fs::path& directoryPath, const fs::path& indexDir) {
    try {
        std::string indexDirStr = indexDir.string();
        lucene::store::Directory* dir = lucene::store::FSDirectory::getDirectory(indexDirStr.c_str(), true);
        lucene::analysis::standard::StandardAnalyzer analyzer;
        lucene::index::IndexWriter writer(dir, &analyzer, true);

        for (fs::recursive_directory_iterator it(directoryPath), end; it != end; ++it) {
            if (fs::is_regular_file(it->status())) {
                const auto& filePath = it->path();
                auto pagesContent = extractTextWithPagesFromFile(filePath);

                std::cout << "Indexing: " << filePath.string() << " (" << pagesContent.size() << " pages)\n";

                for (const auto& [pageNum, content] : pagesContent) {
                    if (!content.empty()) {
                        lucene::document::Document doc;

                        // Путь к файлу - сохраняем как есть
                        std::string pathStr = filePath.string();
                        doc.add(*_CLNEW lucene::document::Field(
                            _T("path"),
                            toTCHAR(pathStr),
                            lucene::document::Field::STORE_YES |
                            lucene::document::Field::INDEX_UNTOKENIZED));

                        // Номер страницы
                        std::string pageStr = std::to_string(pageNum);
                        doc.add(*_CLNEW lucene::document::Field(
                            _T("page"),
                            toTCHAR(pageStr),
                            lucene::document::Field::STORE_YES |
                            lucene::document::Field::INDEX_UNTOKENIZED));

                        // Содержимое для поиска
                        doc.add(*_CLNEW lucene::document::Field(
                            _T("content"),
                            toTCHAR(content),
                            lucene::document::Field::STORE_NO |
                            lucene::document::Field::INDEX_TOKENIZED));

                        writer.addDocument(&doc);
                        std::cout << ".";
                    }
                }
                std::cout << "\n";
            }
        }

        writer.optimize();
        writer.close();
        _CLDELETE(dir);
        std::cout << "Indexing completed successfully.\n";
    }
    catch (const std::exception& e) {
        std::cerr << "Error during indexing: " << e.what() << std::endl;
    }
}


// Поиск по индексу с выводом информации о странице и строке
void searchIndex(const fs::path& indexDir, const std::string& queryStr) {
    try {
        if (!fs::exists(indexDir) || fs::is_empty(indexDir)) {
            std::cerr << "Index is empty or not created. Run 'index' command first." << std::endl;
            return;
        }

        Directory* dir = FSDirectory::getDirectory(indexDir.string().c_str(), false);
        IndexReader* reader = IndexReader::open(dir);
        IndexSearcher searcher(reader);

        lucene::analysis::standard::StandardAnalyzer analyzer;
        lucene::queryParser::QueryParser parser(toTCHAR("content"), &analyzer);

        std::unique_ptr<lucene::search::Query> query(parser.parse(toTCHAR(queryStr)));

        Hits* hits = searcher.search(query.get());
        std::cout << "Found " << hits->length() << " results for \"" << queryStr << "\":" << std::endl;

        for (size_t i = 0; i < hits->length(); ++i) {
            Document& doc = hits->doc(i);

            // Получаем значения полей напрямую
            const TCHAR* path = doc.get(toTCHAR("path"));
            const TCHAR* page = doc.get(toTCHAR("page"));

            if (path) {
                std::cout << (i+1) << ". File: " << TCHAR_TO_STRING(path);
                if (page && _tcslen(page) > 0 && _tcscmp(page, _T("0"))) {
                    std::cout << " (page " << TCHAR_TO_STRING(page) << ")";
                }
                std::cout << std::endl;
            } else {
                std::cout << (i+1) << ". [No path information available]" << std::endl;
            }
        }

        _CLDELETE(hits);
        _CLDELETE(reader);
        _CLDELETE(dir);
    }
    catch (const std::exception& e) {
        std::cerr << "Search error: " << e.what() << std::endl;
    }
}

void printHelp() {
    std::cout << "Usage:\n";
    std::cout << "  index <directory> - Index files in directory\n";
    std::cout << "  search <query>    - Search for files containing query\n";
    std::cout << "  help              - Show this help\n";
    std::cout << "  exit              - Exit program\n";
}

int main() {


    std::string command;
    fs::path dataDir = "test_data";  // Изменено на test_data
    fs::path indexDir = "index";

    std::cout << "Text Indexer (with PDF support)" << std::endl;
    std::cout << "Type 'help' for instructions\n\n" << std::endl;
    std::cout << "Commands: index, search, exit, help" << std::endl;
    while (true) {
        std::cout << "> ";
        std::getline(std::cin, command);
        if (command == "help") {
            printHelp();
        }
        if (command == "index") {
            if (!fs::exists(dataDir)) {
                fs::create_directory(dataDir);
                std::cout << "Created 'test_data' directory. Put your files there and run 'index' again." << std::endl;
            }
            else {
                indexFiles(dataDir, indexDir);
            }
        }
        else if (command == "search") {
            if (!fs::exists(indexDir)) {
                std::cout << "Index not found. Run 'index' first." << std::endl;
            }
            else {
                std::string query;
                std::cout << "Enter search query: ";
                std::getline(std::cin, query);
                searchIndex(indexDir, query);
            }
        }
        else if (command == "exit") {
            break;
        }
        else {
            std::cout << "Unknown command. Use 'index', 'search' or 'exit'." << std::endl;
        }
    }

    return 0;
}
