/*
 * SessionBookdownXRefs.cpp
 *
 * Copyright (C) 2020 by RStudio, PBC
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionBookdownXRefs.hpp"

#include <shared_core/FilePath.hpp>

#include <core/FileSerializer.hpp>
#include <core/Exec.hpp>

#include <core/system/Process.hpp>

#include <r/RExec.hpp>

#include <session/SessionModuleContext.hpp>
#include <session/IncrementalFileChangeHandler.hpp>


namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace bookdown {
namespace xrefs {

using namespace rstudio::core;

namespace {



bool isBookdownRmd(const FileInfo& fileInfo)
{
   FilePath filePath(fileInfo.absolutePath());
   FilePath bookDir = projects::projectContext().buildTargetPath();
   return filePath.isWithin(bookDir) && (filePath.getExtensionLowerCase() == ".rmd");
}

std::vector<std::string> bookdownSourceFiles()
{
   std::vector<std::string> files;
   std::string inputDir = string_utils::utf8ToSystem(projects::projectContext().buildTargetPath().getAbsolutePath());
   Error error = r::exec::RFunction(".rs.bookdown.SourceFiles", inputDir).call(&files);
   if (error)
      LOG_ERROR(error);
   return files;
}


std::string bookRelativePath(const FilePath& rmdFile)
{
   return rmdFile.getRelativePath(projects::projectContext().buildTargetPath());
}

FilePath xrefIndexDirectory()
{
   FilePath xrefsPath = module_context::scopedScratchPath().completeChildPath("bookdown-xrefs");
   Error error = xrefsPath.ensureDirectory();
   if (error)
      LOG_ERROR(error);
   return xrefsPath;
}


FilePath xrefIndexFilePath(const std::string& rmdRelativePath)
{
   FilePath indexFilePath = xrefIndexDirectory().completeChildPath(rmdRelativePath + ".xref");
   Error error = indexFilePath.getParent().ensureDirectory();
   if (error)
      LOG_ERROR(error);
   return indexFilePath;
}

FilePath xrefIndexFilePath(const FilePath& rmdFile)
{
   std::string rmdRelativePath = bookRelativePath(rmdFile);
   return xrefIndexFilePath(rmdRelativePath);
}


struct XRefFileIndex
{
   XRefFileIndex() {}
   explicit XRefFileIndex(const std::string& file) : file(file) {}
   std::string file;
   std::vector<std::string> entries;
};

struct XRefIndexEntry
{
   XRefIndexEntry() {}
   XRefIndexEntry(const std::string& file, const std::string& entry)
      : file(file), entry(entry)
   {
   }
   std::string file;
   std::string entry;
};


XRefFileIndex indexForDoc(const std::string& file, const std::string& contents)
{
   XRefFileIndex index(file);

   // run pandoc w/ custom lua filter to capture index
   std::vector<std::string> args;
   args.push_back("--from");
   args.push_back("markdown-auto_identifiers");
   args.push_back("--to");
   FilePath resPath = session::options().rResourcesPath();
   FilePath xrefLuaPath = resPath.completePath("xref.lua");
   std::string xrefLua = string_utils::utf8ToSystem(xrefLuaPath.getAbsolutePath());
   args.push_back(xrefLua);
   core::system::ProcessResult result;
   Error error = module_context::runPandoc(args, contents, &result);
   if (error)
   {
      LOG_ERROR(error);
   }
   else if (result.exitStatus != EXIT_SUCCESS)
   {
      LOG_ERROR(systemError(boost::system::errc::state_not_recoverable, result.stdErr, ERROR_LOCATION));
   }
   else
   {
      boost::algorithm::split(index.entries, result.stdOut, boost::algorithm::is_any_of("\n"));
   }

   // return the index
   return index;
}

XRefFileIndex indexForDoc(const FilePath& filePath, const std::string& contents)
{
   std::string file = bookRelativePath(filePath);
   return indexForDoc(file, contents);
}



XRefFileIndex indexForDoc(const FilePath& filePath)
{
   std::string contents;
   Error error = core::readStringFromFile(filePath, &contents);
   if (error)
      LOG_ERROR(error);
   return indexForDoc(filePath, contents);
}


void writeEntryId(const std::string& id, json::Object* pEntryJson)
{
   std::size_t colonPos = id.find_first_of(':');
   if (colonPos != std::string::npos)
   {
      pEntryJson->operator[]("type") = id.substr(0, colonPos);
      pEntryJson->operator[]("id") = id.substr(colonPos + 1);
   }
   else
   {
      pEntryJson->operator[]("type") = "";
      pEntryJson->operator[]("id") = id;
   }
}


class XRefUnsavedIndex
{
public:

   const std::map<std::string, XRefFileIndex>& unsavedIndexes(){
      return unsavedFiles_;
   }


   void updateUnsaved(const FileInfo& fileInfo, const std::string& contents, bool dirty)
   {
      // always remove to start with
      removeUnsaved(fileInfo);

      // add it back if it's dirty
      if (dirty)
      {
         FilePath filePath = toFilePath(fileInfo);
         XRefFileIndex idx = indexForDoc(filePath, contents);
         unsavedFiles_[bookRelativePath(filePath)] = idx;
      }
   }

   void removeUnsaved(const FileInfo& fileInfo)
   {
      FilePath filePath = toFilePath(fileInfo);
      unsavedFiles_.erase(bookRelativePath(filePath));

   }

   void removeAllUnsaved()
   {
      unsavedFiles_.clear();
   }

private:
   std::map<std::string, XRefFileIndex> unsavedFiles_;
};
XRefUnsavedIndex s_unsavedIndex;

std::vector<XRefIndexEntry> indexEntriesForProject()
{
   std::vector<XRefIndexEntry> indexEntries;

   // find out what the docs in the book are
   std::vector<std::string> sourceFiles = bookdownSourceFiles();

   for (std::vector<std::string>::size_type i = 0; i < sourceFiles.size(); i++) {

      // alias source files
      const std::string& sourceFile = sourceFiles[i];

      // prefer unsaved files
      std::vector<std::string> entries;
      auto unsaved = s_unsavedIndex.unsavedIndexes();
      std::map<std::string, XRefFileIndex>::const_iterator it = unsaved.find(sourceFile);
      if (it != unsaved.end())
      {
         entries = it->second.entries;
      }
      // then check the disk based index
      else
      {
         FilePath filePath = xrefIndexFilePath(sourceFile);
         if (filePath.exists())
         {
            Error error = readStringVectorFromFile(filePath, &entries);
            if (error)
               LOG_ERROR(error);
         }
      }

      for (auto entry : entries)
      {
         XRefIndexEntry indexEntry(sourceFile, entry);
         indexEntries.push_back(indexEntry);
      }
   }

   return indexEntries;
}

std::vector<XRefIndexEntry> indexEntriesForFile(const XRefFileIndex& fileIndex)
{
   std::vector<XRefIndexEntry> indexEntries;
   for (auto entry : fileIndex.entries)
   {
      XRefIndexEntry indexEntry(fileIndex.file, entry);
      indexEntries.push_back(indexEntry);
   }

   return indexEntries;
}


json::Array indexEntriesToXRefs(const std::vector<XRefIndexEntry>& entries)
{
   // split out text refs (as a map) and normal entries
   std::map<std::string,std::string> textRefs;
   std::vector<XRefIndexEntry> normalEntries;
   boost::regex textRefRe("^(\\(.*\\))\\s+(.*)$");
   for (auto indexEntry : entries)
   {
      boost::smatch matches;
      if (boost::regex_search(indexEntry.entry, matches, textRefRe))
      {
         textRefs[matches[1]] = matches[2];
      }
      else
      {
         normalEntries.push_back(indexEntry);
      }
   }

   // turn normal entries into xref json
   json::Array xrefsJson;
   for (auto indexEntry : normalEntries)
   {
      json::Object xrefJson;

      xrefJson["file"] = indexEntry.file;

      auto entry = indexEntry.entry;
      if (entry.size() > 0)
      {
         std::size_t spacePos = entry.find_first_of(' ');
         if (spacePos != std::string::npos)
         {
            // write the id
            writeEntryId(entry.substr(0, spacePos), &xrefJson);

            // get the title (substitute textref if we have one)
            std::string title = entry.substr(spacePos + 1);

            std::string textrefTitle = textRefs[title];
            if (textrefTitle.length() > 0)
               title = textrefTitle;

            // write the title
            xrefJson["title"] = title;
         }
         else
         {
            writeEntryId(entry, &xrefJson);
         }
         xrefsJson.push_back(xrefJson);
      }
   }

   return xrefsJson;
}



void fileChangeHandler(const core::system::FileChangeEvent& event)
{
   // paths for the rmd file and it's corresponding index file
   FilePath rmdFile = FilePath(event.fileInfo().absolutePath());
   FilePath idxFile = xrefIndexFilePath(FilePath(event.fileInfo().absolutePath()));

   if (event.type() == core::system::FileChangeEvent::FileAdded)
   {
      if (idxFile.exists() && idxFile.getLastWriteTime() > rmdFile.getLastWriteTime())
         return;
   }

   // if this is an add or an update then re-index
   if (event.type() == core::system::FileChangeEvent::FileAdded ||
       event.type() == core::system::FileChangeEvent::FileModified)
   {
      if (rmdFile.exists())
      {
         XRefFileIndex idx = indexForDoc(rmdFile);
         Error error = writeStringVectorToFile(idxFile, idx.entries);
         if (error)
            LOG_ERROR(error);
      }
   }
   // if this is a delete then remove the index
   else if (event.type() == core::system::FileChangeEvent::FileRemoved)
   {
      Error error = idxFile.removeIfExists();
      if (error)
         LOG_ERROR(error);
   }
}



void onSourceDocUpdated(boost::shared_ptr<source_database::SourceDocument> pDoc)
{
   // ignore if the file doesn't have a path
   if (pDoc->path().empty())
      return;

   // update unsaved if it's a bookdown rmd
   FileInfo fileInfo(module_context::resolveAliasedPath(pDoc->path()));
   if (isBookdownRmd(fileInfo))
      s_unsavedIndex.updateUnsaved(fileInfo, pDoc->contents(), pDoc->dirty());

}

void onSourceDocRemoved(const std::string&, const std::string& path)
{
   // ignore if the file has no path
   if (path.empty())
      return;

   // remove from unsaved if it's a bookdown rmd
   FileInfo fileInfo(module_context::resolveAliasedPath(path));
   if (isBookdownRmd(fileInfo))
      s_unsavedIndex.removeUnsaved(fileInfo);
}

void onAllSourceDocsRemoved()
{
   s_unsavedIndex.removeAllUnsaved();
}

bool isBookdownContext()
{
   return module_context::isBookdownWebsite() && module_context::isPackageInstalled("bookdown");
}

void onDeferredInit(bool)
{
   if (isBookdownContext())
   {
      // create an incremental file change handler (on the heap so that it
      // survives the call to this function and is never deleted)
      IncrementalFileChangeHandler* pFileChangeHandler =
         new IncrementalFileChangeHandler(
            isBookdownRmd,
            fileChangeHandler,
            boost::posix_time::seconds(3),
            boost::posix_time::milliseconds(500),
            true
         );
      pFileChangeHandler->subscribeToFileMonitor("Bookdown Cross References");
   }

}


Error xrefIndexForFile(const json::JsonRpcRequest& request,
                       json::JsonRpcResponse* pResponse)
{
   // read params
   std::string file;
   Error error = json::readParams(request.params, &file);
   if (error)
      return error;

   // resolve path
   FilePath filePath = module_context::resolveAliasedPath(file);

   // if this is a bookdown context then send the whole project index
   if (isBookdownContext() && filePath.isWithin(projects::projectContext().buildTargetPath()))
   {
      std::vector<XRefIndexEntry> entries = indexEntriesForProject();
      pResponse->setResult(indexEntriesToXRefs(entries));
   }

   // otherwise just send an index for this file (it will be in the source database)
   else
   {
      std::string id;
      source_database::getId(filePath, &id);
      if (!id.empty())
      {
         boost::shared_ptr<source_database::SourceDocument> pDoc(
                  new source_database::SourceDocument());
         Error error = source_database::get(id, pDoc);
         if (error)
         {
            LOG_ERROR(error);
            pResponse->setResult(json::Array());
         }
         else
         {
            XRefFileIndex idx = indexForDoc(filePath.getFilename(), pDoc->contents());
            std::vector<XRefIndexEntry> entries = indexEntriesForFile(idx);
            pResponse->setResult(indexEntriesToXRefs(entries));
         }
      }
      else
      {
         pResponse->setResult(json::Array());
      }
   }


   return Success();
}

} // anonymous namespace


Error initialize()
{
   // subscribe to source docs events for maintaining the unsaved files list
   source_database::events().onDocUpdated.connect(onSourceDocUpdated);
   source_database::events().onDocRemoved.connect(onSourceDocRemoved);
   source_database::events().onRemoveAll.connect(onAllSourceDocsRemoved);

   // deferred init (build xref file index)
   module_context::events().onDeferredInit.connect(onDeferredInit);

   // register rpc functions
   ExecBlock initBlock;
   initBlock.addFunctions()
     (boost::bind(module_context::registerRpcMethod, "xref_index_for_file", xrefIndexForFile))
   ;
   return initBlock.execute();


}

} // namespace xrefs
} // namespace bookdown
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio
