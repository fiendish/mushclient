#pragma once

#include <string>
#include <vector>

// Entries keep their parent until the event ends, including unseen parents.
struct CTraceOutput
  {
  static const size_t noParent = static_cast<size_t> (-1);
  struct Entry
    {
    size_t parent;
    std::string plugin, message, hiddenCallback;
    unsigned int category;
    bool metadata;
    Entry (size_t p, const std::string & name, unsigned int group,
           const std::string & text = "", bool heading = false) :
      parent (p), plugin (name), message (text), category (group), metadata (heading) {}
    };
  int events;
  size_t parent;
  std::string caller;
  std::vector<Entry> entries;
  CTraceOutput () : events (0), parent (noParent) {}

  std::vector<size_t> DisplayOrder () const
    {
    std::vector<size_t> firstChild (entries.size () + 1, static_cast<size_t> (noParent));
    std::vector<size_t> nextSibling (entries.size (), static_cast<size_t> (noParent));
    for (size_t i = entries.size (); i > 0; --i)
      {
      const size_t ancestor = entries [i - 1].parent == noParent ?
        entries.size () : entries [i - 1].parent;
      nextSibling [i - 1] = firstChild [ancestor];
      firstChild [ancestor] = i - 1;
      }
    std::vector<size_t> order;
    order.reserve (entries.size ());
    for (size_t i = firstChild [entries.size ()]; i != noParent; )
      {
      order.push_back (i);
      if (firstChild [i] != noParent)
        i = firstChild [i];
      else
        {
        while (nextSibling [i] == noParent && entries [i].parent != noParent)
          i = entries [i].parent;
        i = nextSibling [i];
        }
      }
    return order;
    }

  size_t MetadataParent (const std::string & plugin, unsigned int category) const
    {
    size_t ancestor = parent;
      for (size_t i = entries.size (); i > 0; --i)
        if (entries [i - 1].parent == parent && entries [i - 1].plugin == plugin &&
            entries [i - 1].category == category && entries [i - 1].metadata)
          {
          ancestor = i - 1;
          break;
          }
    return ancestor;
    }

  size_t Reserve (const std::string & plugin, unsigned int category, bool continuation)
    {
    entries.push_back (Entry (continuation ? MetadataParent (plugin, category) : parent, plugin, category));
    return entries.size () - 1;
    }

  void Add (const std::string & plugin, unsigned int category, const std::string & message)
    {
    const bool heading = message.find ("Matched alias ") == 0 ||
      message.find ("Matched trigger ") == 0 || message.find ("Fired timer ") == 0 ||
      message.find ("Fired unlabelled timer ") == 0;
    entries.push_back (Entry (parent, plugin, category, message, heading));
    }
  };
