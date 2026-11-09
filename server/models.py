"""Only bounded text is accepted. Audio and client-supplied household IDs are forbidden."""
from typing import Literal
from pydantic import BaseModel, ConfigDict, Field, field_validator


class Strict(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)


class Profile(Strict):
    name: str = Field(min_length=1, max_length=32)
    aliases: list[str] = Field(default_factory=list, max_length=16)
    grade: str = Field(default="", max_length=80)
    level: str = Field(default="", max_length=160)
    interests: str = Field(default="", max_length=160)
    goals: str = Field(default="", max_length=160)

    @field_validator('grade', 'level', 'interests', 'goals')
    @classmethod
    def prompt_length(cls, value):
        if len(value.encode('utf-8')) > 36:
            raise ValueError('profile field exceeds 36 UTF-8 bytes')
        return value


class NotificationSettings(Strict):
    enabled: bool
    time: str = Field(pattern=r'^(?:[01][0-9]|2[0-3]):[0-5][0-9]$')


class Turn(Strict):
    seq: int = Field(ge=0, le=511)
    role: Literal["user", "assistant"]
    text: str = Field(min_length=1, max_length=4096)
    event_id: str = Field(default="", max_length=128)
    item_id: str = Field(default="", max_length=128)
    question_id: str = Field(default="", max_length=128)
    response_id: str = Field(default="", max_length=128)
    content_index: int = Field(default=0, ge=0, le=64)
    incomplete: bool = False
    interrupted: bool = False
    truncated: bool = False
    safety: bool = False


class Upload(Strict):
    session_id: str = Field(pattern=r"^[a-zA-Z0-9_-]{1,80}$")
    child_id: str = Field(pattern=r"^[a-zA-Z0-9_-]{1,80}$")
    generation: int = Field(ge=0)
    profile_version: int = Field(default=1, ge=1)
    started_at: int = Field(ge=0)
    ended_at: int = Field(ge=0)
    complete: bool = True
    turns: list[Turn] = Field(max_length=512)


class Summary(Strict):
    topic: str = Field(max_length=100)
    recap: str = Field(max_length=500)
    resume_question: str = Field(max_length=160)
    new_words: list[str] = Field(max_length=12)
    parent_recap: str = Field(default="未观察到", max_length=60)
    parent_highlight: str = Field(default="未观察到", max_length=60)
    parent_help: str = Field(default="未观察到", max_length=60)

    @field_validator('parent_recap', 'parent_highlight', 'parent_help')
    @classmethod
    def empty_observation(cls, value):
        return value.strip() or '未观察到'


class Login(Strict):
    household: str = Field(max_length=80)
    password: str = Field(max_length=256)


class Ack(Strict):
    generation: int = Field(ge=0)
    status: Literal["ok", "unsaved", "storage_error", "queue_full", "cache_expired"]


class IdentityUtterance(Strict):
    text: str = Field(min_length=1, max_length=160)
