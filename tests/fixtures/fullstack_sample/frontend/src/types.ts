// Shape of GET /api/users (see backend/app/models.py, User.to_dict).
export interface User {
  id: number;
  full_name: string;
  email: string;
}
