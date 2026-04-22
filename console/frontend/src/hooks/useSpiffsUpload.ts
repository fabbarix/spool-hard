import { useMutation } from '@tanstack/react-query';
import { useState } from 'react';
import { uploadWithAuth } from '@spoolhard/ui/utils/uploadWithAuth';

export function useSpiffsUpload() {
  const [progress, setProgress] = useState(0);
  const mutation = useMutation({
    mutationFn: (file: File) => uploadWithAuth('/api/upload/spiffs', file, setProgress),
    onSettled: () => setProgress(0),
  });
  return { ...mutation, progress };
}
