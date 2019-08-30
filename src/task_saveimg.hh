// Handles saving of images

#pragma once
#include "worker.hh"

namespace focusstack {

class Task_SaveImg: public Task
{
public:
  Task_SaveImg(std::string filename, std::shared_ptr<ImgTask> input);

private:
  virtual void task();

  std::shared_ptr<ImgTask> m_input;
};

}
